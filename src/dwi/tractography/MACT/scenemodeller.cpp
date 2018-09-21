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


#include "dwi/tractography/MACT/scenemodeller.h"


#define CUSTOM_PRECISION 1e-5


namespace MR
{

namespace DWI
{

namespace Tractography
{

namespace MACT
{


SceneModeller::SceneModeller( const BoundingBox< double >& boundingBox,
                              const Eigen::Vector3i& lutSize,
                              const Header& header )
              : _boundingBox( boundingBox ),
                _integerBoundingBox( 0, lutSize[ 0 ] - 1,
                                     0, lutSize[ 1 ] - 1,
                                     0, lutSize[ 2 ] - 1 ),
                _lutSize( lutSize ),
                _bresenhamLine( boundingBox, lutSize ),
                _tissueLut( std::shared_ptr< SceneModeller >( this ) ),
                _lutHeader( header )
{
}


SceneModeller::SceneModeller( const BoundingBox< double >& boundingBox,
                              const Eigen::Vector3i& lutSize )
              : _boundingBox( boundingBox ),
                _integerBoundingBox( 0, lutSize[ 0 ] - 1,
                                     0, lutSize[ 1 ] - 1,
                                     0, lutSize[ 2 ] - 1 ),
                _lutSize( lutSize ),
                _bresenhamLine( boundingBox, lutSize ),
                _tissueLut( std::shared_ptr< SceneModeller >( this ) )
{
}


SceneModeller::~SceneModeller()
{
}


const BoundingBox< double >& SceneModeller::boundingBox() const
{
  return _boundingBox;
}


const BoundingBox< int32_t >& SceneModeller::integerBoundingBox() const
{
  return _integerBoundingBox;
}


const Eigen::Vector3i& SceneModeller::lutSize() const
{
  return _lutSize;
}


const BresenhamLine& SceneModeller::bresenhamLine() const
{
  return _bresenhamLine;
}


void SceneModeller::lutVoxel( const Eigen::Vector3d& point,
                              Eigen::Vector3i& voxel ) const
{
  _bresenhamLine.point2voxel( point, voxel );
}


void SceneModeller::addTissues( const std::set< Tissue_ptr >& tissues )
{
  auto t = tissues.begin(), te = tissues.end();
  while ( t != te )
  {
    if ( _tissues.find( ( *t )->type() ) != _tissues.end() )
    {
      throw Exception( "Add duplicate tissue type" );
    }
    _tissues[ ( *t )->type() ] = *t;
    _tissueLut.update( *t );
    ++ t;
  }
}


const TissueLut& SceneModeller::tissueLut() const
{
  return _tissueLut;
}


bool SceneModeller::nearestTissue( const Eigen::Vector3d& point,
                                   Intersection& intersection,
                                   const int32_t& layer ) const
{
  if ( _tissues.empty() )
  {
    return false;
  }
  else
  {
    Eigen::Vector3i voxel;
    _bresenhamLine.point2voxel( point, voxel );

    // unless specified, increase the layer until the nearest tissue is found
    // (min numner of voxels: 3x3x3=27)
    int32_t l = 1;
    do
    {
      std::set< Eigen::Vector3i, Vector3iCompare > voxels;
      if ( l == 1 )
      {
        _bresenhamLine.neighbouringVoxels( voxel, l, voxels );
      }
      else
      {
        _bresenhamLine.layerVoxels( voxel, l, voxels );
      }

      // loop over all unique polygons
      auto tissues = _tissueLut.getTissues( voxels );
      if ( !tissues.empty() )
      {
        auto t = tissues.begin(), te = tissues.end();
        while ( t != te )
        {
          auto polygons = ( *t )->polygonLut().getTriangles( voxels );
          if ( !polygons.empty() )
          {
            auto p = polygons.begin(), pe = polygons.end();
            while ( p != pe )
            {
              auto mesh = ( *t )->mesh();
              Eigen::Vector3d projectionPoint;
              double dist = MACT::pointToTriangleDistance( 
                                                       point,
                                                       mesh.vert( ( *p )[ 0 ] ),
                                                       mesh.vert( ( *p )[ 1 ] ),
                                                       mesh.vert( ( *p )[ 2 ] ),
                                                       projectionPoint );
              if ( dist < intersection._arcLength )
              {
                intersection._arcLength = dist;
                intersection._point = projectionPoint;
                intersection._tissue = *t;
                intersection._triangle = *p;
              }    
              ++ p;
            }
          }
          ++ t;
        }
      }
      ++ l;
    } while ( l < layer && !intersection._tissue );

    return intersection._tissue ? true : false;
  }
}


bool SceneModeller::nearestVertex( const Eigen::Vector3d& point,
                                   int32_t& vertex,
                                   const int32_t& layer ) const
{
  if ( _tissues.empty() )
  {
    return false;
  }
  else
  {
    Eigen::Vector3i voxel;
    _bresenhamLine.point2voxel( point, voxel );

    // unless specified, increase the layer until the nearest tissue is found
    // (min numner of voxels: 3x3x3=27)
    vertex = -1;
    int32_t l = 1;
    do
    {
      std::set< Eigen::Vector3i, Vector3iCompare > voxels;
      if ( l == 1 )
      {
        _bresenhamLine.neighbouringVoxels( voxel, l, voxels );
      }
      else
      {
        _bresenhamLine.layerVoxels( voxel, l, voxels );
      }

      // loop over all unique polygons
      double min_dist = std::numeric_limits< double >::infinity();
      auto tissues = _tissueLut.getTissues( voxels );
      if ( !tissues.empty() )
      {
        auto t = tissues.begin(), te = tissues.end();
        while ( t != te )
        {
          auto polygons = ( *t )->polygonLut().getTriangles( voxels );
          if ( !polygons.empty() )
          {
            auto p = polygons.begin(), pe = polygons.end();
            while ( p != pe )
            {
              auto mesh = ( *t )->mesh();
              for ( size_t v = 0; v < 3; v++ )
              {
                double dist = ( point - mesh.vert( ( *p )[ v ] ) ).norm();
                if ( dist < min_dist )
                {
                  min_dist = dist;
                  vertex = ( *p )[ v ];
                }
              }
              ++ p;
            }
          }
          ++ t;
        }
      }
      ++ l;
    } while ( l < layer && vertex < 0 );

    return ( vertex >= 0 );
  }
}


bool SceneModeller::inTissue( const Eigen::Vector3d& point,
                              const TissueType& type,
                              size_t axis ) const
{
  // ************ Note: the method only functions for a closed mesh ************
  if ( axis > 2 )
  {
    throw Exception( "SceneModeller::inTissue : invalid ray axis" );
  }
  if ( _tissues.find( type ) == _tissues.end()  )
  {
    throw Exception( "Input tissue type not found" );
  }
  else
  {
    auto theTissue = _tissues.find( type )->second;
    double r = _bresenhamLine.minResolution();
    Eigen::Vector3d projectionPoint( point );
    size_t intersectionCount = 0;
    if ( axis == 0 )
    {
      ////// casting a ray in +x or -x direction
      double upperX = _boundingBox.getUpperX();
      double lowerX = _boundingBox.getLowerX();
      projectionPoint[ 0 ] = ( upperX - point[ 0 ] ) < ( point[ 0 ] - lowerX ) ?
                             ( upperX + r ) : ( lowerX - r );
      IntersectionSet iX( *this, point, projectionPoint, theTissue );
      intersectionCount = iX.count();
    }
    if ( axis == 1 )
    {
      ////// casting a ray in +y or -y direction
      projectionPoint = point;
      double upperY = _boundingBox.getUpperY();
      double lowerY = _boundingBox.getLowerY();
      projectionPoint[ 1 ] = ( upperY - point[ 1 ] ) < ( point[ 1 ] - lowerY ) ?
                             ( upperY + r ) : ( lowerY - r );
      IntersectionSet iY( *this, point, projectionPoint, theTissue );
      intersectionCount = iY.count();
    }
    if ( axis == 2 )
    {
      ////// casting a ray in +z or -z direction
      projectionPoint = point;
      double upperZ = _boundingBox.getUpperZ();
      double lowerZ = _boundingBox.getLowerZ();
      projectionPoint[ 2 ] = ( upperZ - point[ 2 ] ) < ( point[ 2 ] - lowerZ ) ?
                             ( upperZ + r ) : ( lowerZ - r );
      IntersectionSet iZ( *this, point, projectionPoint, theTissue );
      intersectionCount = iZ.count();
    }
    // an odd number -> inside ; an even number -> outside
    return ( intersectionCount % 2 ) ? true : false;
  }
}


bool SceneModeller::onTissue( const Eigen::Vector3d& point,
                              const TissueType& type,
                              Intersection& intersection ) const
{
  if ( _tissues.find( type ) == _tissues.end() )
  {
    throw Exception( "Input tissue type not found" );
  }
  else
  {
    nearestTissue( point, intersection, 1 ); /* onyl check 27 neighbours */ 
    if ( intersection._tissue == _tissues.find( type )->second &&
         intersection._arcLength < CUSTOM_PRECISION )
    {
      return true;
    }
    return false;
  }
}


}

}

}

}

