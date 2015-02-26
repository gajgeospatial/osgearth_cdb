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
// CDBTileSource.cpp
//

#include <osgEarth/Registry>
#include <osgEarth/URI>
#include <osgEarth/TileSource>
#include <osgEarth/ImageToHeightFieldConverter>
#include <Windows.h>

#include "CDBTileSource"
#include "CDBOptions"


using namespace osgEarth;


CDBTileSource::CDBTileSource( const osgEarth::TileSourceOptions& options ) : TileSource(options), _options(options), _UseCache(false), _rootDir(""), _cacheDir(""), 
																			_tileSize(1024), _JP2Driver(NULL), _GTIFFDriver(NULL), _CDB_SRS(NULL), _HFADriver(NULL)
{
	//The JP2 Driver Names should be ordered based on read performance however this has not been done yet.
	_JP2DriverNames[0] = "JP2ECW";		//ERDAS supplied JP2 Plugin
	_JP2DriverNames[1] = "JP2OpenJPEG"; //LibOpenJPEG2000
	_JP2DriverNames[2] = "JPEG2000";	//JASPER
	_JP2DriverNames[3] = "JP2KAK";		//Kakadu Library
	_JP2DriverNames[4] = "JP2MrSID";	//MR SID SDK

}   


// CDB uses unprojected lat/lon
osgEarth::TileSource::Status CDBTileSource::initialize(const osgDB::Options* dbOptions)
{
   // No caching of source tiles
   //Note: This is in reference to osgEarth Cacheing. 
   //This driver provides the capablity to cache CDB lower levels of detail. If osgearth caching is used 
   //Then CDB cacheing should not.
   _dbOptions = osgEarth::Registry::instance()->cloneOrCreateOptions( dbOptions );
   osgEarth::CachePolicy::NO_CACHE.apply( _dbOptions.get() );
   // Make sure the root directory is set

   bool errorset = false;
   std::string Errormsg = "";
   //The CDB Root directory is required.
   if (!_options.rootDir().isSet())
   {
	   OE_WARN << "CDB root directory not set!" << std::endl;
	   Errormsg = "CDB root directory not set";
	   errorset = true;
   }
   else
   {
	   _rootDir = _options.rootDir().value();
   }

   //Find a jpeg2000 driver for the image layer.
   int dcount = 0;
   while ((_JP2Driver == NULL) && (dcount < JP2DRIVERCNT))
   {
	   _JP2Driver = GetGDALDriverManager()->GetDriverByName(_JP2DriverNames[dcount].c_str());
	   if (_JP2Driver == NULL)
		   ++dcount;
	   else if (_JP2Driver->pfnOpen == NULL)
	   {
		   _JP2Driver = NULL;
		   ++dcount;
	   }
   }
   if (_JP2Driver == NULL)
   {
	   errorset = true;
	   Errormsg = "No GDAL JP2 Driver Found";
   }

   //Get the GeoTiff driver for the Elevation data
   _GTIFFDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
   if (_GTIFFDriver == NULL)
   {
	   errorset = true;
	   Errormsg = "GDAL GeoTiff Driver Not Found";
   }
   else if (_GTIFFDriver->pfnOpen == NULL)
   {
	   errorset = true;
	   Errormsg = "GDAL GeoTiff Driver has no open function";
   }

   //The Erdas Imagine dirver is currently being used for the
   //Elevation cache files
   _HFADriver = GetGDALDriverManager()->GetDriverByName("HFA");
   if (_HFADriver == NULL)
   {
	   errorset = true;
	   Errormsg = "GDAL ERDAS Imagine Driver Not Found";
   }
   else if (_HFADriver->pfnOpen == NULL)
   {
	   errorset = true;
	   Errormsg = "GDAL ERDAS Imagine Driver has no open function";
   }

   //Get the chache directory if it is set and turn on the cacheing option if it is present
   if (_options.cacheDir().isSet())
   {
	   _cacheDir = _options.cacheDir().value();
	   _UseCache = true;
	   _CDB_SRS = new OGRSpatialReference();
	   _CDB_SRS->SetWellKnownGeogCS("WGS84");
   }

   //verify tilesize
   if (_options.tileSize().isSet())
	   _tileSize = _options.tileSize().value();

   bool profile_set = false;
   int maxcdbdatalevel = 14;
   //Check if there are limits on the maximum Lod to use
   if (_options.MaxCDBLevel().isSet())
   {
	   maxcdbdatalevel = _options.MaxCDBLevel().value();
   }

   //Check to see if we have been told how many negitive lods to use
   int Number_of_Negitive_LODs_to_Use = 0;
   if (_options.NumNegLODs().isSet())
   {
	   Number_of_Negitive_LODs_to_Use = _options.NumNegLODs().value();
   }
 
   //Check to see if we are loading only a limited area of the earth
   if (_options.Limits().isSet())
   {
	   std::string cdbLimits = _options.Limits().value();
	   double	min_lon,
				max_lon,
				min_lat,
				max_lat;

	   int count = sscanf(cdbLimits.c_str(), "%lf,%lf,%lf,%lf", &min_lon, &min_lat, &max_lon, &max_lat);
	   if (count == 4)
	   {
		   //CDB tiles always filter to geocell boundaries
		   min_lon = round(min_lon);
		   min_lat = round(min_lat);
		   max_lat = round(max_lat);
		   max_lon = round(max_lon);
		   //Expand the limits if necessary to meet the criteria for the number of negitive lods specified
		   int subfact = 2 << Number_of_Negitive_LODs_to_Use;  //2 starts with lod 0 this means howerver a minumum of 4 geocells will be requested even if only one
		   if ((max_lon > min_lon) && (max_lat > min_lat))	   //is specified in the limits section of the earth file.
		   {
			   unsigned tiles_x = (unsigned)(max_lon - min_lon);
			   int modx = tiles_x % subfact;
			   if (modx != 0)
			   {
				   tiles_x = ((tiles_x + subfact) / subfact) * subfact;
				   max_lon = min_lon + (double)tiles_x;
			   }
			   tiles_x /= subfact;

			   unsigned tiles_y = (unsigned)(max_lat - min_lat);
			   int mody = tiles_y % subfact;
			   if (mody != 0)
			   {
				   tiles_y = ((tiles_y + subfact) / subfact) * subfact;
				   max_lat = min_lat + (double)tiles_y;
			   }
			   tiles_y /= subfact;

			   //Create the Profile with the calculated limitations
			   osg::ref_ptr<const SpatialReference> src_srs;
			   src_srs = SpatialReference::create("EPSG:4326");
			   GeoExtent extents = GeoExtent(src_srs, min_lon, min_lat, max_lon, max_lat);
			   getDataExtents().push_back(DataExtent(extents, 0, maxcdbdatalevel+Number_of_Negitive_LODs_to_Use+1)); //plus number of sublevels
			   setProfile(osgEarth::Profile::create(src_srs, min_lon, min_lat, max_lon, max_lat, tiles_x, tiles_y));

			   OE_INFO "CDB Profile Min Lon " << min_lon << " Min Lat " << min_lat << " Max Lon " << max_lon << " Max Lat " << max_lat << "Tiles " << tiles_x << " " << tiles_y << std::endl;
			   OE_INFO "  Number of negitive lods " << Number_of_Negitive_LODs_to_Use << " Subfact " << subfact << std::endl;
			   profile_set = true;
		   }
	   }
	   if (!profile_set)
		   OE_WARN << "Invalid Limits received by CDB Driver: Not using Limits" << std::endl;

   }

	   // Always a WGS84 unprojected lat/lon profile.
   if (!profile_set)
   {
	   //Use a default world profile
	   //Note: This will work for small datasets if there is not many geocells of coverage
	   //however if this is a large dataset with a significant portion of the world covered then
	   //using this many top level nodes will take a significant amount of startup time.
	   GeoExtent extents = GeoExtent(SpatialReference::create("EPSG:4326"), -180.0, -90.0, 180.0, 90.0);
	   getDataExtents().push_back(DataExtent(extents, 0, maxcdbdatalevel + 2));
	   setProfile(osgEarth::Profile::create("EPSG:4326", "", 90U, 45U));
   }

   if (errorset)
   {
	   TileSource::Status Rstatus(Errormsg);
	   return Rstatus;
   }
   else
	   return STATUS_OK;
}

#ifdef _MSC_VER
#if _MSC_VER < 1800
double CDBTileSource::round(double x)
{
	return (double)((int)(x + 0.4999999999));
}
#endif
#endif

int CDBTileSource::GetPathComponents(const osgEarth::TileKey& key, bool elevation,
									 std::string& lat_str, std::string& lon_str, std::string& lod_str,
									 std::string& uref_str, std::string& rref_str, std::string &LayerName_str,
									 std::string& dataset_str, std::string &filetype_str, int &LatBase, int &LonBase)
{

	int cdbLod = 0;
	//Get the extents of the tile
	const GeoExtent key_extent = key.getExtent();

	//Determine the CDB LOD
	double keylonspace = key_extent.east() - key_extent.west();
	double keylatspace = key_extent.north() - key_extent.south();

	double tilesperdeg = 1.0 / keylatspace;
	double tilesperdegX = 1.0 / keylonspace;

	if (tilesperdeg < 0.99)
	{
		double lnum = 1.0 / tilesperdeg;
		int itiles = (round(lnum / 2.0));
		cdbLod = -1;
		while (itiles > 1)
		{
			itiles /= 2;
			--cdbLod;
		}

	}
	else
	{
		cdbLod = 0;
		int itiles = (int)round(tilesperdeg);
		while (itiles > 1)
		{
			itiles /= 2;
			++cdbLod;
		}

	}

	int tile_x, tile_y;

	if (cdbLod > 0)
	{
		double degpertileY = 1.0 / tilesperdeg;
		double Base = (double)((int)key_extent.south());
		if (key_extent.south() < Base)
			Base -= 1.0;
		double off = key_extent.south() - Base;
		tile_y = (int)round(off / degpertileY);

		double degpertileX = 1.0 / tilesperdegX;
		Base = (double)((int)key_extent.west());
		if (key_extent.west() < Base)
			Base -= 1.0;
		off = key_extent.west() - Base;
		tile_x = (int)round(off / degpertileX);
	}
	else
	{
		tile_x = tile_y = 0;
	}

	//Determine the base lat lon directory
	double lont = (double)((int)key_extent.west());
	//make sure there wasn't a rounding error
	if (abs((lont + 1.0) - key_extent.west()) < DBL_EPSILON)
		lont += 1.0;
	else if (key_extent.west() < lont)//We're in the Western Hemisphere round down.
		lont -= 1.0;
	LonBase = (int)lont;
	

	double latt = (double)((int)key_extent.south());
	//make sure there wasn't a rounding error
	if (abs((latt + 1.0) - key_extent.south()) < DBL_EPSILON)
		latt += 1.0;
	else if (key_extent.south() < latt)//We're in the Southern Hemisphere round down.
		latt -= 1.0;
	LatBase = (int)latt;

	LatLonstr(LatBase, LonBase, lat_str, lon_str);

	// Set the LOD of the request
	std::stringstream lod_stream;
	if (cdbLod < 0)
		lod_stream << "LC" << std::setfill('0') << std::setw(2) << abs(cdbLod);
	else
		lod_stream << "L" << std::setfill('0') << std::setw(2) << cdbLod;
	lod_str = lod_stream.str();

	if (cdbLod < 1)
	{
		//There is only one tile in cdb levels 0 and below
		//
		uref_str = "U0";
		rref_str = "R0";
	}
	else
	{
		// Determine UREF
		std::stringstream uref_stream;
		uref_stream << "U" << tile_y;
		uref_str = uref_stream.str();

		// Determine RREF
		std::stringstream rref_stream;
		rref_stream << "R" << tile_x;
		rref_str = rref_stream.str();
	}

	if (elevation)
	{
		LayerName_str = "001_Elevation";
		filetype_str = ".tif";
		dataset_str = "_D001_S001_T001_";
	}
	else
	{
		LayerName_str = "004_Imagery";
		filetype_str = ".jp2";
		dataset_str = "_D004_S001_T001_";
	}

	return cdbLod;
}

bool CDBTileSource::Has_Content(int cdbLod, std::string &rootDir, std::string& lod_str,
								std::string& uref_str, std::string& rref_str, std::string &LayerName_str,
								std::string& dataset_str, std::string &filetype_str, int LatBase, int LonBase)

{
	//Determine if there is any content available for the given low resoluton tile
	int tilesXY = 1 << abs(cdbLod);
	bool done = false;

	int cur_col = 0;
	int cur_row = tilesXY - 1;
	int curLat = LatBase;
	int curLon = LonBase;
	bool has_content = false;
	std::string lat_string;
	std::string lon_string;
	LatLonstr(curLat, curLon, lat_string, lon_string);
	std::string base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_str, uref_str,
								   rref_str, LayerName_str, dataset_str, filetype_str);
	while (!done)
	{
		DWORD ftyp = ::GetFileAttributes(base.c_str());
		if (ftyp != INVALID_FILE_ATTRIBUTES)
		{
			//We found content 
			has_content = true;
			break;
		}
		++cur_col;
		if (cur_col >= tilesXY)
		{
			cur_col = 0;
			curLon = LonBase;
			--cur_row;
			if (cur_row < 0)
			{
				done = true;
			}
			else
			{
				++curLat;
			}
		}
		else
		{
			++curLon;
		}
		if (!done)
		{
			//Determin the next filename to check
			LatLonstr(curLat, curLon, lat_string, lon_string);
			base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_str, uref_str,
								rref_str, LayerName_str, dataset_str, filetype_str);
		}
	}
	return has_content;
}

std::string CDBTileSource::CDBFilename(int cdbLod, std::string &rootDir, std::string &lat_str, std::string& lon_str, std::string& lod_str,
									   std::string& uref_str, std::string& rref_str, std::string &LayerName_str,
									   std::string& dataset_str, std::string &filetype_str)
{
	//Construct the name of an elevation or image tile
	std::stringstream buf;
	if (cdbLod >= 0)
	{
		buf << rootDir
			<< "\\Tiles"
			<< "\\" << lat_str
			<< "\\" << lon_str
			<< "\\" << LayerName_str
			<< "\\" << lod_str
			<< "\\" << uref_str
			<< "\\" << lat_str << lon_str << dataset_str << lod_str
			<< "_" << uref_str << "_" << rref_str << filetype_str;
	}
	else
	{
		buf << rootDir
			<< "\\Tiles"
			<< "\\" << lat_str
			<< "\\" << lon_str
			<< "\\" << LayerName_str
			<< "\\LC"
			<< "\\" << uref_str
			<< "\\" << lat_str << lon_str << dataset_str << lod_str
			<< "_" << uref_str << "_" << rref_str << filetype_str;
	}
	return buf.str();
}

std::string CDBTileSource::CDBCachename(int cdbLod, std::string &cacheRootDir, std::string &lat_str, std::string& lon_str, std::string& lod_str,
										std::string& uref_str, std::string& rref_str, std::string &LayerName_str,
										std::string& dataset_str, std::string &filetype_str)
{
	//Construct the name of a cached file name
	std::stringstream buf;
	if (cdbLod >= 0)
	{
		buf << cacheRootDir
			<< "\\Tiles"
			<< "\\" << lat_str
			<< "\\" << lon_str
			<< "\\" << LayerName_str
			<< "\\" << lod_str
			<< "\\" << uref_str
			<< "\\" << lat_str << lon_str << dataset_str << lod_str
			<< "_" << uref_str << "_" << rref_str << filetype_str;
	}
	else
	{
		buf << cacheRootDir
			<< "\\" << LayerName_str
			<< "\\" << lat_str << lon_str << dataset_str << lod_str
			<< "_" << uref_str << "_" << rref_str << filetype_str;
	}
	return buf.str();
}

void CDBTileSource::LatLonstr(int LatBase, int LonBase, std::string &lat_str, std::string &lon_str)
{
	std::stringstream format_stream_1;
	format_stream_1 << ((LonBase < 0) ? "W" : "E") << std::setfill('0')
		<< std::setw(3) << abs(LonBase);
	lon_str = format_stream_1.str();

	std::stringstream format_stream_2;
	format_stream_2 << ((LatBase < 0) ? "S" : "N") << std::setfill('0')
		<< std::setw(2) << abs(LatBase);
	lat_str = format_stream_2.str();

}

bool CDBTileSource::CreateCacheFile(std::string &cacheName, const osgEarth::TileKey& key, void * data, GDALDataType DType, int nband)
{
	//Store a cached tile on disk

	bool successfail = false;
	char **papszOptions = NULL;
	//Create the GeoData Headers
	//Set the transformation Matrix
	double adfGeoTransform[6];
	const GeoExtent key_extent = key.getExtent();

	adfGeoTransform[0] = key_extent.west();
	adfGeoTransform[1] = (key_extent.east() - key_extent.west()) / (double)_tileSize;
	adfGeoTransform[2] = 0.0;
	adfGeoTransform[3] = key_extent.north();
	adfGeoTransform[4] = 0.0;
	adfGeoTransform[5] = ((key_extent.north() - key_extent.south()) / (double)_tileSize) * -1.0;

	GDALDataset * cacheDS = NULL;
	if (nband == 3)
		// We are using GeoTiff to store the Imagery Cache
		cacheDS = _GTIFFDriver->Create(cacheName.c_str(), _tileSize, _tileSize, nband, DType, papszOptions);
	else
		// We are using Erdas Imagine format to store the Elevation cache
		cacheDS = _HFADriver->Create(cacheName.c_str(), _tileSize, _tileSize, nband, DType, papszOptions);

	if (cacheDS)
	{
		cacheDS->SetGeoTransform(adfGeoTransform);
		char *projection = NULL;
		_CDB_SRS->exportToWkt(&projection);
		cacheDS->SetProjection(projection);
		CPLFree(projection);
		CPLErr gdall_err;
		if (nband == 3)
		{
			//Store the Image file
			gdall_err = cacheDS->RasterIO(GF_Write, 0, 0, _tileSize, _tileSize, data, _tileSize, _tileSize, DType, 3, NULL,
										  4, _tileSize * 4, 1);
		}
		else
		{
			//Store the elevation file
			gdall_err = cacheDS->RasterIO(GF_Write, 0, 0, _tileSize, _tileSize, data, _tileSize, _tileSize, DType, 1, NULL,
											0, 0, 0);
		}
		if (gdall_err == CE_Failure)
		{
			OE_INFO "Cache Error writing" << key.str() << "=" << cacheName << std::endl;
			successfail = true;
		}

		GDALClose(cacheDS);
	}
	else
		successfail = true;

	return successfail;
}

osg::Image* CDBTileSource::createImage(const osgEarth::TileKey& key,
										osgEarth::ProgressCallback* progress )
{



   // Build the tile's path
   std::string lat_string, lon_string, lod_string, uref_string, rref_string;
   std::string LayerName_str, filetype_str, dataset_str;

   int LatBase, LonBase;

   int cdbLod = GetPathComponents(key, false, lat_string, lon_string, lod_string, uref_string,
								  rref_string, LayerName_str, dataset_str, filetype_str, LatBase, LonBase); 

   std::string base;
   base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_string, uref_string,
					  rref_string, LayerName_str, dataset_str, filetype_str);
   OE_DEBUG << key.str() << "=" << base << std::endl;

   if (cdbLod >= 0)
   {
	   //We have a single file to check for
	   DWORD ftyp = ::GetFileAttributes(base.c_str());
	   if (ftyp == INVALID_FILE_ATTRIBUTES)
	   {
		   DWORD error = ::GetLastError();

#ifdef _DEBUG
		   OE_INFO "Imagery Not Found" << key.str() << "=" << base << std::endl;
#endif

		   if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			   return NULL;
	   }
   }
   else
   {
	   if (!Has_Content(cdbLod, _rootDir, lod_string, uref_string, rref_string, LayerName_str, dataset_str, filetype_str, LatBase, LonBase))
	   {
#ifdef _DEBUG
		   OE_INFO "Imagery Not Found" << key.str() << "=" << base << std::endl;
#endif
		   return NULL;
	   }
   }
   std::string cacheName = "";
   bool have_cache_file = false;
   if (_UseCache && (cdbLod < 0))
   {
	   //If the usecache option is set see if we have already built and stored a file for the lower level lod
	   std::string cache_type = ".tif";
	   cacheName = CDBCachename(cdbLod, _cacheDir, lat_string, lon_string, lod_string, uref_string,
								rref_string, LayerName_str, dataset_str, cache_type);
	   DWORD ftyp = ::GetFileAttributes(cacheName.c_str());
	   if (ftyp != INVALID_FILE_ATTRIBUTES)
	   {
		   have_cache_file = true;
	   }
   }
//#ifdef _DEBUG
	OE_INFO "Imagery " << key.str() << "=" << base << std::endl;
//#endif


	//allocate the osg image
	osg::ref_ptr<osg::Image> image = new osg::Image;
	GLenum pixelFormat = GL_RGBA;
	image->allocateImage(_tileSize, _tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
	memset(image->data(), 0, image->getImageSizeInBytes());

	if ((cdbLod >= 0) || have_cache_file)
	{

		//Single image direct load to texture
		//ToDo address loading of a partial tile for areas above and below 50 deg.
		GDALDataset *poDataset = NULL;
		if (have_cache_file)
		{
			GDALOpenInfo oOpenInfo(cacheName.c_str(), GA_ReadOnly);
			poDataset = (GDALDataset *)_GTIFFDriver->pfnOpen(&oOpenInfo);
		}
		else
		{
			GDALOpenInfo oOpenInfo(base.c_str(), GA_ReadOnly);
			poDataset = (GDALDataset *)_JP2Driver->pfnOpen(&oOpenInfo);
		}
		if (poDataset)
		{
			int bandoffset = _tileSize * _tileSize;
			//Process Image data
			unsigned char *red = new unsigned char[bandoffset * 3];
			unsigned char *green = red + bandoffset;
			unsigned char *blue = green + bandoffset;
			//Reaad the image data
			CPLErr gdall_err = poDataset->RasterIO(GF_Read, 0, 0, _tileSize, _tileSize, red, _tileSize, _tileSize, GDT_Byte, 3, NULL, 0, 0, 0);

			if (gdall_err != CE_Failure)
			{
				int ibufpos = 0;
				int dst_row = 0;
				for (int iy = 0; iy < _tileSize; ++iy)
				{
					int dst_col = 0;
					for (int ix = 0; ix < _tileSize; ++ix)
					{
						//Populate the osg:image
						*(image->data(dst_col, dst_row) + 0) = red[ibufpos];
						*(image->data(dst_col, dst_row) + 1) = green[ibufpos];
						*(image->data(dst_col, dst_row) + 2) = blue[ibufpos];
						*(image->data(dst_col, dst_row) + 3) = 255;
						++ibufpos;
						++dst_col;
					}
					++dst_row;
				}
			}
			delete red;
			GDALClose(poDataset);
		}
		
	}
	else
	{

		//we have to tile multiple images into the texture
	   int tilesXY = 1 << abs(cdbLod);
	   bool done = false;

	   int cur_col = 0;
	   int cur_row = tilesXY - 1;
	   int curLat = LatBase;
	   int curLon = LonBase;
	   while (!done)
	   {
		   //Read base image
		   GDALDataset * poDataset = NULL;
		   DWORD ftyp = ::GetFileAttributes(base.c_str());
		   if (ftyp != INVALID_FILE_ATTRIBUTES)
		   {
			   GDALOpenInfo oOpenInfo(base.c_str(), GA_ReadOnly);
			   poDataset = (GDALDataset *)_JP2Driver->pfnOpen(&oOpenInfo);
		   }
		   if (poDataset)
		   {
			   int IpixX = poDataset->GetRasterXSize();
			   int IpixY = poDataset->GetRasterYSize();

			   int ObufSX = cur_col * IpixX;
			   int ObufSY = cur_row * IpixY;
			   int bandoffset = IpixX * IpixY;

				//Process Image data
				unsigned char *red = new unsigned char[bandoffset * 3];
				unsigned char *green = red + bandoffset;
				unsigned char *blue = green + bandoffset;

				CPLErr gdall_err = poDataset->RasterIO(GF_Read, 0, 0, IpixX, IpixY, red, IpixX, IpixY, GDT_Byte, 3, NULL, 0, 0, 0);
				if (gdall_err != CE_Failure)
				{
					int ibufpos = 0;
					int dst_row = ObufSY;
					for (int iy = 0; iy < IpixY; ++iy)
					{
						int dst_col = ObufSX;
						for (int ix = 0; ix < IpixX; ++ix)
						{
							*(image->data(dst_col, dst_row) + 0) = red[ibufpos];
							*(image->data(dst_col, dst_row) + 1) = green[ibufpos];
							*(image->data(dst_col, dst_row) + 2) = blue[ibufpos];
							*(image->data(dst_col, dst_row) + 3) = 255;
							++ibufpos;
							++dst_col;
						}
						++dst_row;
					}
				}

				delete red;

			   GDALClose(poDataset);
		   }
		   ++cur_col;
		   if (cur_col >= tilesXY)
		   {
			   cur_col = 0;
			   curLon = LonBase;
			   --cur_row;
			   if (cur_row < 0)
			   {
				   done = true;
			   }
			   else
			   {
				   ++curLat;
			   }
		   }
		   else
		   {
			   ++curLon;
		   }
		   if (!done)
		   {
			   //Get the next filename
			   LatLonstr(curLat, curLon, lat_string, lon_string);
			   base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_string, uref_string,
				   rref_string, LayerName_str, dataset_str, filetype_str);
		   }
		   else
		   {
			   if (_UseCache)
			   {
				   //Save the file as a cache
				   CreateCacheFile(cacheName, key, image->data(0, 0), GDT_Byte, 3);
			   }
		   }
	   }

	}

	image->flipVertical();
	return image.release();

}

osg::HeightField* CDBTileSource::createHeightField(const osgEarth::TileKey& key,
   osgEarth::ProgressCallback* progress )
{


	// Build the tile's path
	std::string lat_string, lon_string, lod_string, uref_string, rref_string;
	std::string LayerName_str, filetype_str, dataset_str;

	int LatBase, LonBase;

	int cdbLod = GetPathComponents(key, true, lat_string, lon_string, lod_string, uref_string,
									rref_string, LayerName_str, dataset_str, filetype_str, LatBase, LonBase);

	std::string base;
	base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_string, uref_string,
						rref_string, LayerName_str, dataset_str, filetype_str);
	OE_DEBUG << key.str() << "=" << base << std::endl;

	if (cdbLod >= 0)
	{
		//We only have one file to consider
		DWORD ftyp = ::GetFileAttributes(base.c_str());
		if (ftyp == INVALID_FILE_ATTRIBUTES)
		{
			DWORD error = ::GetLastError();

#ifdef _DEBUG
			OE_INFO "Elevation Not Found" << key.str() << "=" << base << std::endl;
#endif

			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
				return NULL;
		}
	}
	else
	{
		//build the tile if any cdb tiles that it contains exist
		if (!Has_Content(cdbLod, _rootDir, lod_string, uref_string, rref_string, LayerName_str, dataset_str, filetype_str, LatBase, LonBase))
		{
#ifdef _DEBUG
			OE_INFO "Elevation Not Found" << key.str() << "=" << base << std::endl;
#endif
			return NULL;
		}
	}

	//#ifdef _DEBUG
	OE_INFO "Elevation " << key.str() << "=" << base << std::endl;
	//#endif

	std::string cacheName = "";
	bool have_cache_file = false;
	if (_UseCache && (cdbLod < 0))
	{
		//If the usecache option is set see if we have already built and stored a file for the lower level lod
		std::string cache_type = ".img";
		cacheName = CDBCachename(cdbLod, _cacheDir, lat_string, lon_string, lod_string, uref_string,
			rref_string, LayerName_str, dataset_str, cache_type);
		DWORD ftyp = ::GetFileAttributes(cacheName.c_str());
		if (ftyp != INVALID_FILE_ATTRIBUTES)
		{
			have_cache_file = true;
		}
	}


	//Allocate the heightfield
	osg::ref_ptr<osg::HeightField> field = new osg::HeightField;
	field->allocate(_tileSize, _tileSize);
	//For now clear the data
	for (unsigned int i = 0; i < field->getHeightList().size(); ++i) 
		field->getHeightList()[i] = NO_DATA_VALUE;

	if (cdbLod >= 0 || have_cache_file)
	{
		//The data is contained in a single file
		//ToDo Handle case where the data is only a partial file when Latitudes are above are below 50 deg
		GDALDataset *poDataset = NULL;
		if (have_cache_file)
		{
			GDALOpenInfo oOpenInfo(cacheName.c_str(), GA_ReadOnly);
			poDataset = (GDALDataset *)_HFADriver->pfnOpen(&oOpenInfo);
		}
		else
		{
			GDALOpenInfo oOpenInfo(base.c_str(), GA_ReadOnly);
			poDataset = (GDALDataset *)_GTIFFDriver->pfnOpen(&oOpenInfo);
		}
		if (poDataset)
		{
			float *elevation = new float[_tileSize*_tileSize];

			CPLErr gdall_err = poDataset->RasterIO(GF_Read, 0, 0, _tileSize, _tileSize, elevation, _tileSize, _tileSize, GDT_Float32, 1, NULL, 0, 0, 0);
			if (gdall_err != CE_Failure)
			{
				int ibpos = 0;
				for (unsigned int r = 0; r < (unsigned)_tileSize; r++)
				{
					unsigned inv_r = _tileSize - r - 1;
					for (unsigned int c = 0; c < (unsigned)_tileSize; c++)
					{
						//Re-enable this is data is suspect
#if 0
						float h = elevation[ibpos];
						// Mark the value as nodata using the universal NO_DATA_VALUE marker.
						if (!isValidValue(h, band))
						{
							h = NO_DATA_VALUE;
						}
#endif
						field->setHeight(c, inv_r, elevation[ibpos]);
						++ibpos;
					}
				}

			}
			delete elevation;
			GDALClose(poDataset);
		}
	}
	else
	{
		//We need to compose the tile from multiple tiles
		int tilesXY = 1 << abs(cdbLod);
		bool done = false;
		int cur_col = 0;
		int cur_row = tilesXY - 1;
		int curLat = LatBase;
		int curLon = LonBase;
		while (!done)
		{
			//Read base image
			GDALDataset * poDataset = NULL;
			DWORD ftyp = ::GetFileAttributes(base.c_str());
			if (ftyp != INVALID_FILE_ATTRIBUTES)
			{
				GDALOpenInfo oOpenInfo(base.c_str(), GA_ReadOnly);
				poDataset = (GDALDataset *)_GTIFFDriver->pfnOpen(&oOpenInfo);
			}
			if (poDataset)
			{
				int IpixX = poDataset->GetRasterXSize();
				int IpixY = poDataset->GetRasterYSize();

				int ObufSX = cur_col * IpixX;
				int ObufSY = cur_row * IpixY;
				int bandoffset = IpixX * IpixY;
				//Process Elevation Data

				float *elevation = new float[bandoffset];

				CPLErr gdall_err = poDataset->RasterIO(GF_Read, 0, 0, IpixX, IpixY, elevation, IpixX, IpixY, GDT_Float32, 1, NULL, 0, 0, 0);
				if (gdall_err != CE_Failure)
				{
					int ibufpos = 0;
					int dst_row = ObufSY;
					for (int iy = 0; iy < IpixY; ++iy)
					{
						int dst_col = ObufSX;
						unsigned int inv_r = _tileSize - dst_row - 1;
						//Height field rows are in reverse order of how they are stored in the GIS raster space
						for (int ix = 0; ix < IpixX; ++ix)
						{
							field->setHeight(dst_col, inv_r, elevation[ibufpos]);
							++ibufpos;
							++dst_col;
						}
						++dst_row;
					}
				}
				delete elevation;
				GDALClose(poDataset);
			}
			++cur_col;
			if (cur_col >= tilesXY)
			{
				cur_col = 0;
				curLon = LonBase;
				--cur_row;
				if (cur_row < 0)
				{
					done = true;
				}
				else
				{
					++curLat;
				}
			}
			else
			{
				++curLon;
			}
			if (!done)
			{
				//Get the next File to include
				LatLonstr(curLat, curLon, lat_string, lon_string);
				base = CDBFilename(cdbLod, _rootDir, lat_string, lon_string, lod_string, uref_string,
								   rref_string, LayerName_str, dataset_str, filetype_str);
			}
			else
			{
				if (_UseCache)
				{
					//Save the compositied data as a cached file
					//Reverse the rows before writeing so that the file may be used in geospatial viewers
					//for debug purpouses
					unsigned int isize = field->getHeightList().size();
					float *outElev = new float[isize];
					unsigned int outIdx = 0;
					unsigned int inStartIdx = (unsigned int)(_tileSize - 1);
					for (int line = 0; line < _tileSize; ++line)
					{
						for (unsigned int Pix = 0; Pix < (unsigned int)_tileSize; ++Pix)
						{
							outElev[outIdx] = field->getHeight(Pix, inStartIdx);
							++outIdx;
						}
						--inStartIdx;
					}

					CreateCacheFile(cacheName, key, outElev, GDT_Float32, 1);
					delete outElev;
				}
			}
		}
	}

	return field.release();
}

std::string CDBTileSource::getExtension()  const 
{
   return "jp2";
}

/** Tell the terrain engine not to cache tiles from this source. */
osgEarth::CachePolicy CDBTileSource::getCachePolicyHint() const
{
   return osgEarth::CachePolicy::NO_CACHE;
}
