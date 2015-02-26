// Copyright (c) 1994-2013 Georgia Tech Research Corporation, Atlanta, GA
// This file is part of FalconView(tm).

// FalconView(tm) is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// FalconView(tm) is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public License
// along with FalconView(tm).  If not, see <http://www.gnu.org/licenses/>.

// FalconView(tm) is a trademark of Georgia Tech Research Corporation.
// 2014-2015 GAJ Geospatial Enterprises, Orlando FL
// Modified for General Incorporation of Common Database (CDB) support within osgEarth

// CDBTileSourceDriver.cpp
//

#include <osgDB\FileNameUtils>

#include "CDBTileSource"
#include "CDBTileSourceDriver"

CDBTileSourceDriver::CDBTileSourceDriver()
{
   supportsExtension( "osgearth_cdb", "Common Database" );
}

const char* CDBTileSourceDriver::className()
{
   return "Common Database ReaderWriter";
}

osgDB::ReaderWriter::ReadResult CDBTileSourceDriver::readObject(
   const std::string& file_name, const Options* options) const
{
   if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
         return osgDB::ReaderWriter::ReadResult::FILE_NOT_HANDLED;

   return new CDBTileSource( getTileSourceOptions(options) );
}

REGISTER_OSGPLUGIN(osgearth_cdb, CDBTileSourceDriver)
