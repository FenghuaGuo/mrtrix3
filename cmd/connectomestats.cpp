/*
 * Copyright (c) 2008-2018 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/
 *
 * MRtrix3 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/
 */


#include "command.h"
#include "progressbar.h"
#include "types.h"

#include "file/path.h"
#include "math/stats/fwe.h"
#include "math/stats/glm.h"
#include "math/stats/import.h"
#include "math/stats/shuffle.h"
#include "math/stats/typedefs.h"

#include "connectome/enhance.h"
#include "connectome/mat2vec.h"

#include "stats/permtest.h"


using namespace MR;
using namespace App;
using namespace MR::Math::Stats;
using namespace MR::Math::Stats::GLM;

using Math::Stats::matrix_type;
using Math::Stats::vector_type;
using Stats::PermTest::count_matrix_type;


const char* algorithms[] = { "nbs", "nbse", "none", nullptr };



// TODO Eventually these will move to some kind of TFCE header
#define TFCE_DH_DEFAULT 0.1
#define TFCE_E_DEFAULT 0.4
#define TFCE_H_DEFAULT 3.0

#define EMPIRICAL_SKEW_DEFAULT 1.0



void usage ()
{
  AUTHOR = "Robert E. Smith (robert.smith@florey.edu.au)";

  SYNOPSIS = "Connectome group-wise statistics at the edge level using non-parametric permutation testing";

  DESCRIPTION
      + Math::Stats::GLM::column_ones_description
      + Math::Stats::GLM::sqrt_f_description;


  ARGUMENTS
  + Argument ("input", "a text file listing the file names of the input connectomes").type_file_in ()

  + Argument ("algorithm", "the algorithm to use in network-based clustering/enhancement. "
                           "Options are: " + join(algorithms, ", ")).type_choice (algorithms)

  + Argument ("design", "the design matrix").type_file_in ()

  + Argument ("contrast", "the contrast matrix").type_file_in ()

  + Argument ("output", "the filename prefix for all output.").type_text();


  OPTIONS

  + Math::Stats::shuffle_options (true, EMPIRICAL_SKEW_DEFAULT)

  // TODO OptionGroup these, and provide a generic loader function
  + Stats::TFCE::Options (TFCE_DH_DEFAULT, TFCE_E_DEFAULT, TFCE_H_DEFAULT)

  + Math::Stats::GLM::glm_options ("edge")

  + OptionGroup ("Additional options for connectomestats")

  + Option ("threshold", "the t-statistic value to use in threshold-based clustering algorithms")
  + Argument ("value").type_float (0.0);

  REFERENCES + "* If using the NBS algorithm: \n"
               "Zalesky, A.; Fornito, A. & Bullmore, E. T. Network-based statistic: Identifying differences in brain networks. \n"
               "NeuroImage, 2010, 53, 1197-1207"

             + "* If using the NBSE algorithm: \n"
               "Vinokur, L.; Zalesky, A.; Raffelt, D.; Smith, R.E. & Connelly, A. A Novel Threshold-Free Network-Based Statistics Method: Demonstration using Simulated Pathology. \n"
               "OHBM, 2015, 4144"

             + "* If using the -nonstationary option: \n"
               "Salimi-Khorshidi, G.; Smith, S.M. & Nichols, T.E. Adjusting the effect of nonstationarity in cluster-based and TFCE inference. \n"
               "Neuroimage, 2011, 54(3), 2006-19";

}




void load_tfce_parameters (Stats::TFCE::Wrapper& enhancer)
{
  const default_type dH = get_option_value ("tfce_dh", TFCE_DH_DEFAULT);
  const default_type E  = get_option_value ("tfce_e",  TFCE_E_DEFAULT);
  const default_type H  = get_option_value ("tfce_h",  TFCE_H_DEFAULT);
  enhancer.set_tfce_parameters (dH, E, H);
}



// Define data importer class that will obtain connectome data for a
//   specific subject based on the string path to the image file for
//   that subject
class SubjectConnectomeImport : public SubjectDataImportBase
{ MEMALIGN(SubjectConnectomeImport)
  public:
    SubjectConnectomeImport (const std::string& path) :
        SubjectDataImportBase (path)
    {
      auto M = load_matrix (path);
      Connectome::check (M);
      if (Connectome::is_directed (M))
        throw Exception ("Connectome from file \"" + Path::basename (path) + "\" is a directed matrix");
      Connectome::to_upper (M);
      Connectome::Mat2Vec mat2vec (M.rows());
      mat2vec.M2V (M, data);
    }

    void operator() (matrix_type::RowXpr row) const override
    {
      assert (row.size() == data.size());
      row = data;
    }

    default_type operator[] (const size_t index) const override
    {
      assert (index < size_t(data.size()));
      return (data[index]);
    }

    size_t size() const override { return data.size(); }

  private:
    vector_type data;

};



void run()
{

  // Read file names and check files exist
  CohortDataImport importer;
  importer.initialise<SubjectConnectomeImport> (argument[0]);
  CONSOLE ("Number of subjects: " + str(importer.size()));
  const size_t num_edges = importer[0]->size();

  for (size_t i = 1; i < importer.size(); ++i) {
    if (importer[i]->size() != importer[0]->size())
      throw Exception ("Size of connectome for subject " + str(i) + " (file \"" + importer[i]->name() + "\" does not match that of first subject");
  }

  // TODO Could determine this from the vector length with the right equation
  const MR::Connectome::matrix_type example_connectome = load_matrix (importer[0]->name());
  const MR::Connectome::node_t num_nodes = example_connectome.rows();
  Connectome::Mat2Vec mat2vec (num_nodes);

  // Initialise enhancement algorithm
  std::shared_ptr<Stats::EnhancerBase> enhancer;
  switch (int(argument[1])) {
    case 0: {
      auto opt = get_options ("threshold");
      if (!opt.size())
        throw Exception ("For NBS algorithm, -threshold option must be provided");
      enhancer.reset (new MR::Connectome::Enhance::NBS (num_nodes, opt[0][0]));
      }
      break;
    case 1: {
      std::shared_ptr<Stats::TFCE::EnhancerBase> base (new MR::Connectome::Enhance::NBS (num_nodes));
      enhancer.reset (new Stats::TFCE::Wrapper (base));
      load_tfce_parameters (*(dynamic_cast<Stats::TFCE::Wrapper*>(enhancer.get())));
      if (get_options ("threshold").size())
        WARN (std::string (argument[1]) + " is a threshold-free algorithm; -threshold option ignored");
      }
      break;
    case 2: {
      enhancer.reset (new MR::Connectome::Enhance::PassThrough());
      if (get_options ("threshold").size())
        WARN ("No enhancement algorithm being used; -threshold option ignored");
      }
      break;
    default:
      throw Exception ("Unknown enhancement algorithm");
  }

  const bool do_nonstationarity_adjustment = get_options ("nonstationarity").size();
  const default_type empirical_skew = get_option_value ("skew_nonstationarity", EMPIRICAL_SKEW_DEFAULT);

  // Load design matrix
  const matrix_type design = load_matrix (argument[2]);
  if (size_t(design.rows()) != importer.size())
    throw Exception ("number of subjects (" + str(importer.size()) + ") does not match number of rows in design matrix (" + str(design.rows()) + ")");

  // Before validating the contrast matrix, we first need to see if there are any
  //   additional design matrix columns coming from edge-wise subject data
  vector<CohortDataImport> extra_columns;
  bool nans_in_columns = false;
  auto opt = get_options ("column");
  for (size_t i = 0; i != opt.size(); ++i) {
    extra_columns.push_back (CohortDataImport());
    extra_columns[i].initialise<SubjectConnectomeImport> (opt[i][0]);
    if (!extra_columns[i].allFinite())
      nans_in_columns = true;
  }
  const ssize_t num_factors = design.cols() + extra_columns.size();
  CONSOLE ("Number of factors: " + str(num_factors));
  if (extra_columns.size()) {
    CONSOLE ("Number of element-wise design matrix columns: " + str(extra_columns.size()));
    if (nans_in_columns)
      CONSOLE ("Non-finite values detected in element-wise design matrix columns; individual rows will be removed from edge-wise design matrices accordingly");
  }
  check_design (design, extra_columns.size());

  // Load hypotheses
  const vector<Hypothesis> hypotheses = Math::Stats::GLM::load_hypotheses (argument[3]);
  const size_t num_hypotheses = hypotheses.size();
  if (hypotheses[0].cols() != num_factors)
    throw Exception ("the number of columns in the contrast matrix (" + str(hypotheses[0].cols()) + ")"
                     + " does not equal the number of columns in the design matrix (" + str(design.cols()) + ")"
                     + (extra_columns.size() ? " (taking into account the " + str(extra_columns.size()) + " uses of -column)" : ""));
  CONSOLE ("Number of hypotheses: " + str(num_hypotheses));

  const std::string output_prefix = argument[4];

  // Load input data
  // For compatibility with existing statistics code, symmetric matrix data is adjusted
  //   into vector form - one row per edge in the symmetric connectome. This has already
  //   been performed when the CohortDataImport class is initialised.
  matrix_type data (importer.size(), num_edges);
  {
    ProgressBar progress ("Agglomerating input connectome data", importer.size());
    for (size_t subject = 0; subject < importer.size(); subject++) {
      (*importer[subject]) (data.row (subject));
      ++progress;
    }
  }
  const bool nans_in_data = data.allFinite();

  // Only add contrast matrix row number to image outputs if there's more than one hypothesis
  auto postfix = [&] (const size_t i) { return (num_hypotheses > 1) ? ("_" + hypotheses[i].name()) : ""; };

  {
    matrix_type betas (num_factors, num_edges);
    matrix_type abs_effect_size (num_edges, num_hypotheses), std_effect_size (num_edges, num_hypotheses);
    vector_type cond (num_edges), stdev (num_edges);

    Math::Stats::GLM::all_stats (data, design, extra_columns, hypotheses,
                                 cond, betas, abs_effect_size, std_effect_size, stdev);

    ProgressBar progress ("outputting beta coefficients, effect size and standard deviation", num_factors + (2 * num_hypotheses) + 1 + (nans_in_data || extra_columns.size() ? 1 : 0));
    for (ssize_t i = 0; i != num_factors; ++i) {
      save_matrix (mat2vec.V2M (betas.row(i)), "beta" + str(i) + ".csv");
      ++progress;
    }
    for (size_t i = 0; i != num_hypotheses; ++i) {
      if (!hypotheses[i].is_F()) {
        save_matrix (mat2vec.V2M (abs_effect_size.col(i)), "abs_effect" + postfix(i) + ".csv"); ++progress;
        save_matrix (mat2vec.V2M (std_effect_size.col(i)), "std_effect" + postfix(i) + ".csv"); ++progress;
      }
    }
    if (nans_in_data || extra_columns.size()) {
      save_matrix (mat2vec.V2M (cond), "cond.csv");
      ++progress;
    }
    save_matrix (mat2vec.V2M (stdev), "std_dev.csv");
  }

  // Construct the class for performing the initial statistical tests
  std::shared_ptr<GLM::TestBase> glm_test;
  if (extra_columns.size() || nans_in_data) {
    glm_test.reset (new GLM::TestVariable (extra_columns, data, design, hypotheses, nans_in_data, nans_in_columns));
  } else {
    glm_test.reset (new GLM::TestFixed (data, design, hypotheses));
  }

  // If performing non-stationarity adjustment we need to pre-compute the empirical statistic
  matrix_type empirical_statistic;
  if (do_nonstationarity_adjustment) {
    Stats::PermTest::precompute_empirical_stat (glm_test, enhancer, empirical_skew, empirical_statistic);
    for (size_t i = 0; i != num_hypotheses; ++i)
      save_matrix (mat2vec.V2M (empirical_statistic.col(i)), output_prefix + "empirical" + postfix(i) + ".csv");
  }

  // Precompute default statistic and enhanced statistic
  matrix_type default_output, enhanced_output;
  Stats::PermTest::precompute_default_permutation (glm_test, enhancer, empirical_statistic, enhanced_output, default_output);
  for (size_t i = 0; i != num_hypotheses; ++i) {
    if (hypotheses[i].is_F())
      save_matrix (mat2vec.V2M (default_output.col(i).array().square()), output_prefix + "Fvalue" + postfix(i) + ".csv");
    else
      save_matrix (mat2vec.V2M (default_output.col(i)), output_prefix + "tvalue" + postfix(i) + ".csv");
    save_matrix (mat2vec.V2M (enhanced_output.col(i)), output_prefix + "enhanced" + postfix(i) + ".csv");
  }

  // Perform permutation testing
  if (!get_options ("notest").size()) {

    const bool fwe_strong = get_option_value ("strong", false);
    if (fwe_strong && num_hypotheses == 1) {
      WARN("Option -strong has no effect when testing a single hypothesis only");
    }

    matrix_type null_distribution, uncorrected_pvalues;
    count_matrix_type null_contributions;
    Stats::PermTest::run_permutations (glm_test, enhancer, empirical_statistic, enhanced_output, fwe_strong,
                                       null_distribution, null_contributions, uncorrected_pvalues);
    if (fwe_strong) {
      save_vector (null_distribution.col(0), output_prefix + "null_dist.txt");
    } else {
      for (size_t i = 0; i != num_hypotheses; ++i)
        save_vector (null_distribution.col(i), output_prefix + "null_dist" + postfix(i) + ".txt");
    }

    const matrix_type pvalue_output = MR::Math::Stats::fwe_pvalue (null_distribution, enhanced_output);
    for (size_t i = 0; i != num_hypotheses; ++i) {
      save_matrix (mat2vec.V2M (pvalue_output.col(i)),       output_prefix + "fwe_pvalue" + postfix(i) + ".csv");
      save_matrix (mat2vec.V2M (uncorrected_pvalues.col(i)), output_prefix + "uncorrected_pvalue" + postfix(i) + ".csv");
      save_matrix (mat2vec.V2M (null_contributions.col(i)),  output_prefix + "null_contributions" + postfix(i) + ".csv");
    }

  }

}
