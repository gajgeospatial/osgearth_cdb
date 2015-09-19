// Copyright (c) 2014-2015 GAJ Geospatial Enterprises, Orlando FL
// This file is based on the Common Database (CDB) Specification for USSOCOM
// Version 3.0 – October 2008

// CDB_Tile is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// CDB_Tile is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public License
// along with CDB_Tile.  If not, see <http://www.gnu.org/licenses/>.

// 2015 GAJ Geospatial Enterprises, Orlando FL
// Modified for General Incorporation of Common Database (CDB) support within osgEarth
//
#include "CDB_Tile"
#include <Windows.h>

#define GEOTRSFRM_TOPLEFT_X            0
#define GEOTRSFRM_WE_RES               1
#define GEOTRSFRM_ROTATION_PARAM1      2
#define GEOTRSFRM_TOPLEFT_Y            3
#define GEOTRSFRM_ROTATION_PARAM2      4
#define GEOTRSFRM_NS_RES               5

#define JP2DRIVERCNT 5

CDB_GDAL_Drivers Gbl_TileDrivers;

const int Gbl_CDB_Tile_Sizes[11] = {1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1};
//Caution this only goes down to CDB Level 17
const double Gbl_CDB_Tiles_Per_LOD[18] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0, 16384.0, 32768.0, 65536.0, 131072.0};

CDB_Tile::CDB_Tile(std::string cdbRootDir, std::string cdbCacheDir, CDB_Tile_Type TileType, CDB_Tile_Extent *TileExtent, int NLod) : m_cdbRootDir(cdbRootDir), m_cdbCacheDir(cdbCacheDir),
				   m_TileExtent(*TileExtent), m_TileType(TileType), m_ImageContent_Status(NotSet), m_Tile_Status(Created), m_FileName(""), m_LayerName(""), m_FileExists(false),
				   m_CDB_LOD_Num(0)
{
	if (NLod > 0)
	{
		m_Pixels.pixX = Gbl_CDB_Tile_Sizes[NLod];
		m_Pixels.pixY = Gbl_CDB_Tile_Sizes[NLod]; 
		m_CDB_LOD_Num = -NLod;
	}

	std::string			lat_str,
						lon_str,
						lod_str,
						uref_str,
						rref_str;
	std::stringstream	buf;

	m_CDB_LOD_Num = GetPathComponents(lat_str, lon_str, lod_str, uref_str, rref_str);

	std::string filetype;
	std::string datasetstr;

	if (m_TileType == Elevation)
	{
		m_LayerName = "001_Elevation";
		if (m_CDB_LOD_Num < 0 && NLod == 0)
		{
			m_TileType = ElevationCache;
			filetype = ".img";
		}
		else
		{
			filetype = ".tif";
		}

		datasetstr = "_D001_S001_T001_";
		m_Pixels.pixType = GDT_Float32;
		m_Pixels.bands = 1;
	}
	else if (m_TileType == Imagery)
	{
		m_LayerName = "004_Imagery";
		if (m_CDB_LOD_Num < 0 && NLod == 0)
		{
			m_TileType = ImageryCache;
			filetype = ".tif";
		}
		else
		{
			filetype = ".jp2";
		}
		datasetstr = "_D004_S001_T001_";
	}
	else
	{
		m_TileType = CDB_Unknown;
		filetype = ".unk";
		datasetstr = "_DUNK_SUNK_TUNK_";
	}

	//Set tile size for lower levels of detail

	if (m_CDB_LOD_Num < 0)
	{
		if (NLod == 0)
		{
			buf << cdbCacheDir
				<< "\\" << m_LayerName
				<< "\\" << lat_str << lon_str << datasetstr << lod_str
				<< "_" << uref_str << "_" << rref_str << filetype;
		}
		else
		{
			buf << cdbRootDir
				<< "\\Tiles"
				<< "\\" << lat_str
				<< "\\" << lon_str
				<< "\\" << m_LayerName
				<< "\\LC"
				<< "\\" << uref_str
				<< "\\" << lat_str << lon_str << datasetstr << lod_str
				<< "_" << uref_str << "_" << rref_str << filetype;
		}
	}
	else
	{
		buf << cdbRootDir
			<< "\\Tiles"
			<< "\\" << lat_str
			<< "\\" << lon_str
			<< "\\" << m_LayerName
			<< "\\" << lod_str
			<< "\\" << uref_str
			<< "\\" << lat_str << lon_str << datasetstr << lod_str
			<< "_" << uref_str << "_" << rref_str << filetype;
	}
	m_FileName = buf.str();

	DWORD fstatus = ::GetFileAttributes(m_FileName.c_str());
	if (fstatus == INVALID_FILE_ATTRIBUTES)
		m_FileExists = false;
	else
		m_FileExists = true;

	m_Pixels.degPerPix.Xpos = (m_TileExtent.East - m_TileExtent.West) / (double)(m_Pixels.pixX);
	m_Pixels.degPerPix.Ypos = (m_TileExtent.North - m_TileExtent.South) / (double)(m_Pixels.pixY);


}


CDB_Tile::~CDB_Tile()
{
	Close_Dataset();

	Free_Buffers();
}

std::string CDB_Tile::FileName(void)
{
	return m_FileName;
}

int CDB_Tile::CDB_LOD_Num(void)
{
	return m_CDB_LOD_Num;
}

void CDB_Tile::Free_Resources(void)
{
	Close_Dataset();

	Free_Buffers();
}

int CDB_Tile::GetPathComponents(std::string& lat_str, std::string& lon_str, std::string& lod_str,
								std::string& uref_str, std::string& rref_str)
{

	int cdbLod = 0;

	//Determine the CDB LOD
	double keylonspace = m_TileExtent.East - m_TileExtent.West;
	double keylatspace = m_TileExtent.North - m_TileExtent.South;

	double tilesperdeg = 1.0 / keylatspace;
	double tilesperdegX = 1.0 / keylonspace;

	if (tilesperdeg < 0.99)
	{
		//This is a multi-tile cash tile
		double lnum = 1.0 / tilesperdeg;
		int itiles = (int)(round(lnum / 2.0));
		cdbLod = -1;
		while (itiles > 1)
		{
			itiles /= 2;
			--cdbLod;
		}
	}
	else
	{
		if (m_Pixels.pixX < 1024)
		{
			// In this case the lod num has already been passed into the class
			// just use it.
			cdbLod = m_CDB_LOD_Num;
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
	}
	int tile_x, tile_y;

	double Base_lon = m_TileExtent.West;

	if (cdbLod > 0)
	{
		double Base_lat = (double)((int)m_TileExtent.South);
		if (m_TileExtent.South < Base_lat)
			Base_lat -= 1.0;
		double off = m_TileExtent.South - Base_lat;
		tile_y = (int)round(off * tilesperdeg);

		double lon_step = Get_Lon_Step(m_TileExtent.South);
		Base_lon = (double)((int)m_TileExtent.West);

		if (lon_step != 1.0)
		{
			int checklon = (int)abs(Base_lon);
			if (checklon % (int)lon_step)
			{
				double sign = Base_lon < 0.0 ? -1.0 : 1.0;
				checklon = (checklon / (int)lon_step) * (int)lon_step;
				Base_lon = (double)checklon * sign;
				if (sign < 0.0)
					Base_lon -= lon_step;
			}
		}

		if (m_TileExtent.West < Base_lon)
			Base_lon -= lon_step;

		off = m_TileExtent.West - Base_lon;
		tile_x = (int)round(off * tilesperdegX);
	}
	else
	{
		tile_x = tile_y = 0;
	}
	//Determine the base lat lon directory
	double lont = (double)((int)Base_lon);
	//make sure there wasn't a rounding error
	if (abs((lont + 1.0) - Base_lon) < DBL_EPSILON)
		lont += 1.0;
	else if (Base_lon < lont)//Where in the Western Hemisphere round down.
		lont -= 1.0;


	int londir = (int)lont;
	std::stringstream format_stream_1;
	format_stream_1 << ((londir < 0) ? "W" : "E") << std::setfill('0')
		<< std::setw(3) << abs(londir);
	lon_str = format_stream_1.str();


	double latt = (double)((int)m_TileExtent.South);
	//make sure there wasn't a rounding error
	if (abs((latt + 1.0) - m_TileExtent.South) < DBL_EPSILON)
		latt += 1.0;
	else if (m_TileExtent.South < latt) //Where in the Southern Hemisphere round down.
		latt -= 1.0;

	int latdir = (int)latt;
	std::stringstream format_stream_2;
	format_stream_2 << ((latdir < 0) ? "S" : "N") << std::setfill('0')
		<< std::setw(2) << abs(latdir);
	lat_str = format_stream_2.str();

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
	return cdbLod;
}


void CDB_Tile::Allocate_Buffers(void)
{
	int bandbuffersize = m_Pixels.pixX * m_Pixels.pixY;
	if ((m_TileType == Imagery) || (m_TileType == ImageryCache))
	{
		if (!m_GDAL.reddata)
		{
			m_GDAL.reddata = new unsigned char[bandbuffersize * 3];
			m_GDAL.greendata = m_GDAL.reddata + bandbuffersize;
			m_GDAL.bluedata = m_GDAL.greendata + bandbuffersize;
		}
	}
	else if ((m_TileType == Elevation) || (m_TileType == ElevationCache))
	{
		if (!m_GDAL.elevationdata)
			m_GDAL.elevationdata = new float[bandbuffersize];
	}
}

void CDB_Tile::Free_Buffers(void)
{
	if (m_GDAL.reddata)
	{
		delete m_GDAL.reddata;
		m_GDAL.reddata = NULL;
		m_GDAL.greendata = NULL;
		m_GDAL.bluedata = NULL;
	}
	if (m_GDAL.elevationdata)
	{
		delete m_GDAL.elevationdata;
		m_GDAL.elevationdata = NULL;
	}
	if (m_Tile_Status == Loaded)
	{
		if (m_GDAL.poDataset)
			m_Tile_Status = Opened;
		else
			m_Tile_Status = Created;
	}
}

void CDB_Tile::Close_Dataset(void)
{

	if (m_GDAL.poDataset)
	{
		GDALClose(m_GDAL.poDataset);
		m_GDAL.poDataset = NULL;
	}
	m_Tile_Status = Created;

}

bool CDB_Tile::Open_Tile(void)
{
	if (m_GDAL.poDataset)
		return true;

	GDALOpenInfo oOpenInfo(m_FileName.c_str(), GA_ReadOnly);
	if (m_TileType == Imagery)
	{
		m_GDAL.poDriver = Gbl_TileDrivers.cdb_JP2Driver;
	}
	else if (m_TileType == ImageryCache)
	{
		m_GDAL.poDriver = Gbl_TileDrivers.cdb_GTIFFDriver;
	}
	else if (m_TileType == Elevation)
	{
		m_GDAL.poDriver = Gbl_TileDrivers.cdb_GTIFFDriver;
	}
	else if (m_TileType == ElevationCache)
	{
		m_GDAL.poDriver = Gbl_TileDrivers.cdb_HFADriver;
	}
	m_GDAL.poDataset = (GDALDataset *)m_GDAL.poDriver->pfnOpen(&oOpenInfo);

	if (!m_GDAL.poDataset)
	{
		return false;
	}
	m_GDAL.poDataset->GetGeoTransform(m_GDAL.adfGeoTransform);
	m_Tile_Status = Opened;

	return true;
}

bool CDB_Tile::Read(void)
{
	if (!m_GDAL.poDataset)
		return false;

	if (m_Tile_Status == Loaded)
		return true;


	if ((m_TileType == Imagery) || (m_TileType == ImageryCache))
	{
		CPLErr gdal_err = m_GDAL.poDataset->RasterIO(GF_Read, 0, 0, m_Pixels.pixX, m_Pixels.pixY,
													 m_GDAL.reddata, m_Pixels.pixX, m_Pixels.pixY, GDT_Byte, 3, NULL, 0, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}

	}
	else if ((m_TileType == Elevation) || (m_TileType == ElevationCache))
	{
		GDALRasterBand * ElevationBand = m_GDAL.poDataset->GetRasterBand(1);

		CPLErr gdal_err = ElevationBand->RasterIO(GF_Read, 0, 0, m_Pixels.pixX, m_Pixels.pixY,
			                                      m_GDAL.elevationdata, m_Pixels.pixX, m_Pixels.pixY, GDT_Float32, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}
	}
	m_Tile_Status = Loaded;

	return true;
}

void CDB_Tile::Fill_Tile(void)
{
	int buffsz = m_Pixels.pixX * m_Pixels.pixY;
	if (m_TileType == Imagery)
	{
		for (int i = 0; i < buffsz; ++i)
		{
			m_GDAL.reddata[i] = 127;
			m_GDAL.greendata[i] = 127;
			m_GDAL.bluedata[i] = 127;
		}
	}
	else if ((m_TileType == Elevation) || (m_TileType == ElevationCache))
	{
		for (int i = 0; i < buffsz; ++i)
		{
			m_GDAL.elevationdata[i] = 0.0f;
		}

	}
	else if (m_TileType == ImageryCache)
	{
		for (int i = 0; i < buffsz; ++i)
		{
			m_GDAL.reddata[i] = 0;
			m_GDAL.greendata[i] = 0;
			m_GDAL.bluedata[i] = 0;
		}
	}
}

bool CDB_Tile::Tile_Exists(void)
{
	return m_FileExists;
}

bool CDB_Tile::Build_Cache_Tile(bool save_cache)
{
	//This is not actually part of the CDB specification but
	//necessary to support an osgEarth global profile
	//Build a list of the tiles to use for this cache tile

	double MinLat = m_TileExtent.South;
	double MinLon = m_TileExtent.West;
	double MaxLat = m_TileExtent.North;
	double MaxLon = m_TileExtent.East;

	CDB_Tile_Type subTileType;
	if (m_TileType == ImageryCache)
	{
		subTileType = Imagery;
	}
	else if (m_TileType == ElevationCache)
	{
		subTileType = Elevation;
	}
	CDB_TilePV Tiles;

	bool done = false;
	CDB_Tile_Extent thisTileExtent;
	thisTileExtent.West = MinLon;
	thisTileExtent.South = MinLat;
	double lonstep = Get_Lon_Step(thisTileExtent.South);
	double sign;
	while (!done)
	{
		if (lonstep != 1.0)
		{
			int checklon = (int)abs(thisTileExtent.West);
			if (checklon % (int)lonstep)
			{
				sign = thisTileExtent.West < 0.0 ? -1.0 : 1.0;
				checklon = (checklon / (int)lonstep) * (int)lonstep;
				thisTileExtent.West = (double)checklon * sign;
				if (sign < 0.0)
					thisTileExtent.West -= lonstep;
			}
		}
		thisTileExtent.East = thisTileExtent.West + lonstep;
		thisTileExtent.North = thisTileExtent.South + 1.0;

		CDB_TileP LodTile = new CDB_Tile(m_cdbRootDir, m_cdbCacheDir, subTileType, &thisTileExtent, m_CDB_LOD_Num);

		if (LodTile->Tile_Exists())
		{
			Tiles.push_back(LodTile);
		}
		else
			delete LodTile;

		thisTileExtent.West += lonstep;
		if (thisTileExtent.West > MaxLon)
		{
			thisTileExtent.West = MinLon;
			thisTileExtent.South += 1.0;
			lonstep = Get_Lon_Step(thisTileExtent.South);
			if (thisTileExtent.South > MaxLat)
				done = true;
		}
	}

	int tilecnt = (int)Tiles.size();
	if (tilecnt <= 0)
		return false;

	Build_From_Tiles(&Tiles);

	if (save_cache && (m_Tile_Status == Loaded))
	{
		Save();
		m_FileExists = true;
	}

	//Clean up
	for each (CDB_TileP Tile in Tiles)
	{
		if (Tile)
		{
			delete Tile;
			Tile = NULL;
		}
	}
	Tiles.clear();

	return true;
}

bool CDB_Tile::Build_Earth_Tile(void)
{
	//Build an Earth Profile tile for Latitudes above and below 50 deg

	double MinLon = m_TileExtent.West;

	CDB_Tile_Type subTileType = m_TileType;

	CDB_TilePV Tiles;

	bool done = false;

	OE_DEBUG "Build_Earth_Tile with " << m_FileName.c_str() << std::endl;
	OE_DEBUG "Build_Earth_Tile my extent " << m_TileExtent.North << " " <<m_TileExtent.South << " " << m_TileExtent.East << " " << m_TileExtent.West << std::endl;

	double lonstep = Get_Lon_Step(m_TileExtent.South);
	lonstep /= Gbl_CDB_Tiles_Per_LOD[m_CDB_LOD_Num];

	double incrs = round(MinLon / lonstep);
	double test = incrs * lonstep;
	if (test != MinLon)
	{
		MinLon = test;
	}

	CDB_Tile_Extent thisTileExtent;
	thisTileExtent.West = MinLon;
	thisTileExtent.South = m_TileExtent.South;
	thisTileExtent.East = thisTileExtent.West + lonstep;
	thisTileExtent.North = m_TileExtent.North;

	OE_DEBUG "Build_Earth_Tile cdb extent " << thisTileExtent.North << " " << thisTileExtent.South << " " << thisTileExtent.East << " " << thisTileExtent.West << std::endl;

	//Now get the actual cdb tile with the correct CDB extents
	CDB_TileP LodTile = new CDB_Tile(m_cdbRootDir, m_cdbCacheDir, subTileType, &thisTileExtent);

	OE_DEBUG "Build_Earth_Tile cdb tile " << LodTile->FileName().c_str() << std::endl;

	if (LodTile->Tile_Exists())
	{
		OE_DEBUG "CDB_Tile found " << LodTile->FileName().c_str() << std::endl;
		Tiles.push_back(LodTile);
	}
	else
		delete LodTile;

	int tilecnt = (int)Tiles.size();
	//if the tile does not exist
	//there is no tile to build
	if (tilecnt <= 0)
		return false;

	//Build the osgearth tile from the cdb tile
	Build_From_Tiles(&Tiles, true);

	//clean up
	for each (CDB_TileP Tile in Tiles)
	{
		if (Tile)
		{
			delete Tile;
			Tile = NULL;
		}
	}
	Tiles.clear();

	return true;
}

double CDB_Tile::Get_Lon_Step(double Latitude)
{
	double test = abs(Latitude);
	double step;
	if (Latitude >= 0.0)
	{
		if (test < 50.0)
			step = 1.0;
		else if (test >= 50.0 && test < 70.0)
			step = 2.0;
		else if (test >= 70.0 && test < 75.0)
			step = 3.0;
		else if (test >= 75.0 && test < 80.0)
			step = 4.0;
		else if (test >= 80.0 && test < 89.0)
			step = 6.0;
		else
			step = 12.0;
	}
	else
	{
		if (test <= 50.0)
			step = 1.0;
		else if (test > 50.0 && test <= 70.0)
			step = 2.0;
		else if (test > 70.0 && test <= 75.0)
			step = 3.0;
		else if (test > 75.0 && test <= 80.0)
			step = 4.0;
		else if (test > 80.0 && test <= 89.0)
			step = 6.0;
		else
			step = 12.0;

	}
	return step;
}

bool CDB_Tile::Initialize_Tile_Drivers(std::string &ErrorMsg)
{
	ErrorMsg = "";
	if (Gbl_TileDrivers.cdb_drivers_initialized)
		return true;

	std::string	cdb_JP2DriverNames[JP2DRIVERCNT];
	//The JP2 Driver Names should be ordered based on read performance however this has not been done yet.
	cdb_JP2DriverNames[0] = "JP2ECW";		//ERDAS supplied JP2 Plugin
	cdb_JP2DriverNames[1] = "JP2OpenJPEG";  //LibOpenJPEG2000
	cdb_JP2DriverNames[2] = "JPEG2000";	    //JASPER
	cdb_JP2DriverNames[3] = "JP2KAK";		//Kakadu Library
	cdb_JP2DriverNames[4] = "JP2MrSID";	    //MR SID SDK

	//Find a jpeg2000 driver for the image layer.
	int dcount = 0;
	while ((Gbl_TileDrivers.cdb_JP2Driver == NULL) && (dcount < JP2DRIVERCNT))
	{
		Gbl_TileDrivers.cdb_JP2Driver = GetGDALDriverManager()->GetDriverByName(cdb_JP2DriverNames[dcount].c_str());
		if (Gbl_TileDrivers.cdb_JP2Driver == NULL)
			++dcount;
		else if (Gbl_TileDrivers.cdb_JP2Driver->pfnOpen == NULL)
		{
			Gbl_TileDrivers.cdb_JP2Driver = NULL;
			++dcount;
		}
	}
	if (Gbl_TileDrivers.cdb_JP2Driver == NULL)
	{
		ErrorMsg = "No GDAL JP2 Driver Found";
		return false;
	}

	//Get the GeoTiff driver for the Elevation data
	Gbl_TileDrivers.cdb_GTIFFDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
	if (Gbl_TileDrivers.cdb_GTIFFDriver == NULL)
	{
		ErrorMsg = "GDAL GeoTiff Driver Not Found";
		return false;
	}
	else if (Gbl_TileDrivers.cdb_GTIFFDriver->pfnOpen == NULL)
	{
		ErrorMsg = "GDAL GeoTiff Driver has no open function";
		return false;
	}

	//The Erdas Imagine dirver is currently being used for the
	//Elevation cache files
	Gbl_TileDrivers.cdb_HFADriver = GetGDALDriverManager()->GetDriverByName("HFA");
	if (Gbl_TileDrivers.cdb_HFADriver == NULL)
	{
		ErrorMsg = "GDAL ERDAS Imagine Driver Not Found";
		return false;
	}
	else if (Gbl_TileDrivers.cdb_HFADriver->pfnOpen == NULL)
	{
		ErrorMsg = "GDAL ERDAS Imagine Driver has no open function";
		return false;
	}

	Gbl_TileDrivers.cdb_drivers_initialized = true;
	return true;
}

Image_Contrib CDB_Tile::Get_Contribution(CDB_Tile_Extent &TileExtent)
{
	//Does the Image fall entirly within the Tile
	Image_Contrib retval = Image_Is_Inside_Tile(TileExtent);
	if (retval != None)
		return retval;

	//Check the tile contents against the Image
	if ((TileExtent.North < m_TileExtent.South) || (TileExtent.South > m_TileExtent.North) ||
		(TileExtent.East < m_TileExtent.West) || (TileExtent.West > m_TileExtent.East))
		retval = None;
	else if ((TileExtent.North <= m_TileExtent.North) && (TileExtent.South >= m_TileExtent.South) &&
		(TileExtent.East <= m_TileExtent.East) && (TileExtent.West >= m_TileExtent.West))
	{
		retval = Full;
	}
	else
		retval = Partial;

	return retval;
}

Image_Contrib CDB_Tile::Image_Is_Inside_Tile(CDB_Tile_Extent &TileExtent)
{
	int count = 0;
	coord2d point;
	point.Xpos = m_TileExtent.West;
	point.Ypos = m_TileExtent.North;
	if (Point_is_Inside_Tile(point, TileExtent))
		++count;

	point.Xpos = m_TileExtent.East;
	if (Point_is_Inside_Tile(point, TileExtent))
		++count;

	point.Ypos = m_TileExtent.South;
	if (Point_is_Inside_Tile(point, TileExtent))
		++count;

	point.Xpos = m_TileExtent.West;
	if (Point_is_Inside_Tile(point, TileExtent))
		++count;

	if (count > 0)
		return Partial;
	else
		return None;
}

bool CDB_Tile::Point_is_Inside_Tile(coord2d &Point, CDB_Tile_Extent &TileExtent)
{
	if ((Point.Xpos < TileExtent.East) && (Point.Xpos > TileExtent.West) && (Point.Ypos > TileExtent.South) && (Point.Ypos < TileExtent.North))
		return true;
	else
		return false;
}

bool CDB_Tile::Load_Tile(void)
{
	if (m_Tile_Status == Loaded)
		return true;

	if (!m_FileExists)
		return false;

	Allocate_Buffers();

	if (!Open_Tile())
		return false;

	if (!Read())
		return false;

	return true;
}

coord2d CDB_Tile::LL2Pix(coord2d LLPoint)
{
	coord2d PixCoord;
	if ((m_Tile_Status == Loaded) || (m_Tile_Status == Opened))
	{
		double xRel = LLPoint.Xpos - m_TileExtent.West;
		double yRel = m_TileExtent.North - LLPoint.Ypos;
		PixCoord.Xpos = xRel / m_GDAL.adfGeoTransform[GEOTRSFRM_WE_RES];
		PixCoord.Ypos = yRel / abs(m_GDAL.adfGeoTransform[GEOTRSFRM_NS_RES]);
	}
	else
	{
		PixCoord.Xpos = -1.0;
		PixCoord.Ypos = -1.0;
	}
	return PixCoord;
}

bool CDB_Tile::Get_Image_Pixel(coord2d ImPix, unsigned char &RedPix, unsigned char &GreenPix, unsigned char &BluePix)
{
	int tx = (int)ImPix.Xpos;
	int ty = (int)ImPix.Ypos;

	if ((tx < 0) || (tx > m_Pixels.pixX - 1) || (ty < 0) || (ty > m_Pixels.pixY - 1))
	{
		return false;
	}

	int bpos1 = (((int)ImPix.Ypos) * m_Pixels.pixX) + (int)ImPix.Xpos;
	int bpos2 = bpos1 + 1;
	int bpos3 = bpos1 + (int)m_Pixels.pixX;
	int bpos4 = bpos3 + 1;

	if (tx == m_Pixels.pixX - 1)
	{
		bpos2 = bpos1;
		bpos4 = bpos3;
	}

	if (ty == m_Pixels.pixY - 1)
	{
		bpos3 = bpos1;
		if (tx == m_Pixels.pixX - 1)
			bpos4 = bpos1;
		else
			bpos4 = bpos2;
	}

	float rat2 = (float)(ImPix.Xpos - double(tx));
	float rat1 = 1.0f - rat2;
	float rat4 = (float)(ImPix.Ypos - double(ty));
	float rat3 = 1.0f - rat4;

	float p1p = ((float)m_GDAL.reddata[bpos1] * rat1) + ((float)m_GDAL.reddata[bpos2] * rat2);
	float p2p = ((float)m_GDAL.reddata[bpos3] * rat1) + ((float)m_GDAL.reddata[bpos4] * rat2);
	float red = (p1p * rat3) + (p2p * rat4);

	p1p = ((float)m_GDAL.greendata[bpos1] * rat1) + ((float)m_GDAL.greendata[bpos2] * rat2);
	p2p = ((float)m_GDAL.greendata[bpos3] * rat1) + ((float)m_GDAL.greendata[bpos4] * rat2);
	float green = (p1p * rat3) + (p2p * rat4);

	p1p = ((float)m_GDAL.bluedata[bpos1] * rat1) + ((float)m_GDAL.bluedata[bpos2] * rat2);
	p2p = ((float)m_GDAL.bluedata[bpos3] * rat1) + ((float)m_GDAL.bluedata[bpos4] * rat2);
	float blue = (p1p * rat3) + (p2p * rat4);

	red = round(red) < 255.0f ? round(red) : 255.0f;
	green = round(green) < 255.0f ? round(green) : 255.0f;
	blue = round(blue) < 255.0f ? round(blue) : 255.0f;

	RedPix = (unsigned char)red;
	GreenPix = (unsigned char)green;
	BluePix = (unsigned char)blue;
	return true;

}

bool CDB_Tile::Get_Elevation_Pixel(coord2d ImPix, float &ElevationPix)
{
	int tx = (int)ImPix.Xpos;
	int ty = (int)ImPix.Ypos;

	if ((tx < 0) || (tx > m_Pixels.pixX - 1) || (ty < 0) || (ty > m_Pixels.pixY - 1))
	{
		return false;
	}

	int bpos1 = (((int)ImPix.Ypos) * m_Pixels.pixX) + (int)ImPix.Xpos;
	int bpos2 = bpos1 + 1;
	int bpos3 = bpos1 + (int)m_Pixels.pixX;
	int bpos4 = bpos3 + 1;

	if (tx == m_Pixels.pixX - 1)
	{
		bpos2 = bpos1;
		bpos4 = bpos3;
	}

	if (ty == m_Pixels.pixY - 1)
	{
		bpos3 = bpos1;
		if (tx == m_Pixels.pixX - 1)
			bpos4 = bpos1;
		else
			bpos4 = bpos2;
	}

	float rat2 = (float)(ImPix.Xpos - double(tx));
	float rat1 = 1.0f - rat2;
	float rat4 = (float)(ImPix.Ypos - double(ty));
	float rat3 = 1.0f - rat4;

	float e1p = (m_GDAL.elevationdata[bpos1] * rat1) + (m_GDAL.elevationdata[bpos2] * rat2);
	float e2p = (m_GDAL.elevationdata[bpos3] * rat1) + (m_GDAL.elevationdata[bpos4] * rat2);
	float elevation = (e1p * rat3) + (e2p * rat4);

	ElevationPix = elevation;
	return true;
}

double CDB_Tile::West(void)
{
	return m_TileExtent.West;
}

double CDB_Tile::East(void)
{
	return m_TileExtent.East;
}

double CDB_Tile::North(void)
{
	return m_TileExtent.North;
}

double CDB_Tile::South(void)
{
	return m_TileExtent.South;
}

void CDB_Tile::Build_From_Tiles(CDB_TilePV *Tiles, bool from_scratch)
{

	Allocate_Buffers();

	if (!from_scratch && m_FileExists)
	{
		Open_Tile();
		//Load the current tile Information
		Read();
	}
	else
	{
		Fill_Tile();
	}

	bool have_some_contribution = false;
	double XRes = (m_TileExtent.East - m_TileExtent.West) / (double)m_Pixels.pixX;
	double YRes = (m_TileExtent.North - m_TileExtent.South) / (double)m_Pixels.pixY;
	for each (CDB_TileP tile in *Tiles)
	{
		Image_Contrib ImageContrib = tile->Get_Contribution(m_TileExtent);
		if ((ImageContrib == Full) || (ImageContrib == Partial))
		{
			if (tile->Load_Tile())
			{
				have_some_contribution = true;
				int sy = (int)((m_TileExtent.North - tile->North()) / YRes);
				if (sy < 0)
					sy = 0;
				int ey = (int)((m_TileExtent.North - tile->South()) / YRes);
				if (ey > m_Pixels.pixY - 1)
					ey = m_Pixels.pixY - 1;
				int sx = (int)((tile->West() - m_TileExtent.West) / XRes);
				if (sx < 0)
					sx = 0;
				int ex = (int)((tile->East() - m_TileExtent.West) / XRes);
				if (ex > m_Pixels.pixX - 1)
					ex = m_Pixels.pixX - 1;

				double srowlon = m_TileExtent.West + ((double)sx * XRes);
				double srowlat = m_TileExtent.North - ((double)sy *  YRes);
				int buffloc = 0;
				coord2d clatlon;
				clatlon.Ypos = srowlat;
				for (int iy = sy; iy <= ey; ++iy)
				{
					buffloc = (iy * m_Pixels.pixX) + sx;
					clatlon.Xpos = srowlon;
					for (int ix = sx; ix <= ex; ++ix)
					{
						coord2d impix = tile->LL2Pix(clatlon);
						if ((m_TileType == Imagery) || (m_TileType == ImageryCache))
						{
							unsigned char redpix, greenpix, bluepix;
							if (tile->Get_Image_Pixel(impix, redpix, greenpix, bluepix))
							{
								m_GDAL.reddata[buffloc] = redpix;
								m_GDAL.greendata[buffloc] = greenpix;
								m_GDAL.bluedata[buffloc] = bluepix;
							}
						}
						else if ((m_TileType == Elevation) || (m_TileType == ElevationCache))
						{
							tile->Get_Elevation_Pixel(impix, m_GDAL.elevationdata[buffloc]);
						}
						++buffloc;
						clatlon.Xpos += XRes;
					}
					clatlon.Ypos -= YRes;
				}
			}
			tile->Free_Resources();

		}
	}

	if (have_some_contribution)
	{
		m_Tile_Status = Loaded;
	}

}

bool CDB_Tile::Save(void)
{
	char **papszOptions = NULL;

	//Set the transformation Matrix
	m_GDAL.adfGeoTransform[0] = m_TileExtent.West;
	m_GDAL.adfGeoTransform[1] = (m_TileExtent.East - m_TileExtent.West) / (double)m_Pixels.pixX;
	m_GDAL.adfGeoTransform[2] = 0.0;
	m_GDAL.adfGeoTransform[3] = m_TileExtent.North;
	m_GDAL.adfGeoTransform[4] = 0.0;
	m_GDAL.adfGeoTransform[5] = ((m_TileExtent.North - m_TileExtent.South) / (double)m_Pixels.pixY) * -1.0;

	OGRSpatialReference *CDB_SRS = new OGRSpatialReference();
	CDB_SRS->SetWellKnownGeogCS("WGS84");
	if (m_GDAL.poDataset)
	{
		Close_Dataset();
	}


	if (m_TileType == ImageryCache)
	{
		if (m_GDAL.poDataset == NULL)
		{
			//Get the Imagine Driver
			m_GDAL.poDriver = Gbl_TileDrivers.cdb_GTIFFDriver;
			GDALDataType dataType = GDT_Byte;
			if (!m_GDAL.poDriver)
			{
				delete CDB_SRS;
				return false;
			}
			//Create the file
			m_GDAL.poDataset = m_GDAL.poDriver->Create(m_FileName.c_str(), m_Pixels.pixX, m_Pixels.pixY, m_Pixels.bands, dataType, papszOptions);

			if (!m_GDAL.poDataset)
			{
				delete CDB_SRS;
				return false;
			}

			m_GDAL.poDataset->SetGeoTransform(m_GDAL.adfGeoTransform);
			char *projection = NULL;
			CDB_SRS->exportToWkt(&projection);
			m_GDAL.poDataset->SetProjection(projection);
			CPLFree(projection);
		}
		//Write the elevation data to the file
		if (!Write())
		{
			delete CDB_SRS;
			return false;
		}
	}
	else if (m_TileType == ElevationCache)
	{
		if (m_GDAL.poDataset == NULL)
		{
			//Get the Imagine Driver
			m_GDAL.poDriver = Gbl_TileDrivers.cdb_HFADriver;
			GDALDataType dataType = GDT_Float32;
			if (!m_GDAL.poDriver)
			{
				delete CDB_SRS;
				return false;
			}
			//Create the file
			m_GDAL.poDataset = m_GDAL.poDriver->Create(m_FileName.c_str(), m_Pixels.pixX, m_Pixels.pixY, m_Pixels.bands, dataType, papszOptions);

			if (!m_GDAL.poDataset)
			{
				delete CDB_SRS;
				return false;
			}
			m_GDAL.poDataset->SetGeoTransform(m_GDAL.adfGeoTransform);
			char *projection = NULL;
			CDB_SRS->exportToWkt(&projection);
			m_GDAL.poDataset->SetProjection(projection);
			CPLFree(projection);
		}
		//Write the elevation data to the file
		if (!Write())
		{
			delete CDB_SRS;
			return false;
		}
	}

	delete CDB_SRS;
	return true;
}

bool CDB_Tile::Write(void)
{
	CPLErr gdal_err;

	if (m_TileType == ImageryCache)
	{
		GDALRasterBand * RedBand = m_GDAL.poDataset->GetRasterBand(1);
		GDALRasterBand * GreenBand = m_GDAL.poDataset->GetRasterBand(2);
		GDALRasterBand * BlueBand = m_GDAL.poDataset->GetRasterBand(3);

		gdal_err = RedBand->RasterIO(GF_Write, 0, 0, m_Pixels.pixX, m_Pixels.pixY, m_GDAL.reddata, m_Pixels.pixX, m_Pixels.pixY, GDT_Byte, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}

		gdal_err = GreenBand->RasterIO(GF_Write, 0, 0, m_Pixels.pixX, m_Pixels.pixY, m_GDAL.greendata, m_Pixels.pixX, m_Pixels.pixY, GDT_Byte, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}

		gdal_err = BlueBand->RasterIO(GF_Write, 0, 0, m_Pixels.pixX, m_Pixels.pixY, m_GDAL.bluedata, m_Pixels.pixX, m_Pixels.pixY, GDT_Byte, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}
	}
	else if (m_TileType == ElevationCache)
	{

		GDALRasterBand * ElevationBand = m_GDAL.poDataset->GetRasterBand(1);
		gdal_err = ElevationBand->RasterIO(GF_Write, 0, 0, m_Pixels.pixX, m_Pixels.pixY, m_GDAL.elevationdata, m_Pixels.pixX, m_Pixels.pixY, GDT_Float32, 0, 0);
		if (gdal_err == CE_Failure)
		{
			return false;
		}
	}
	return true;
}

osg::Image* CDB_Tile::Image_From_Tile(void)
{
	if (m_Tile_Status == Loaded)
	{
		//allocate the osg image
		osg::ref_ptr<osg::Image> image = new osg::Image;
		GLenum pixelFormat = GL_RGBA;
		image->allocateImage(m_Pixels.pixX, m_Pixels.pixY, 1, pixelFormat, GL_UNSIGNED_BYTE);
		memset(image->data(), 0, image->getImageSizeInBytes());
		int ibufpos = 0;
		int dst_row = 0;
		for (int iy = 0; iy < m_Pixels.pixY; ++iy)
		{
			int dst_col = 0;
			for (int ix = 0; ix < m_Pixels.pixX; ++ix)
			{
				//Populate the osg:image
				*(image->data(dst_col, dst_row) + 0) = m_GDAL.reddata[ibufpos];
				*(image->data(dst_col, dst_row) + 1) = m_GDAL.greendata[ibufpos];
				*(image->data(dst_col, dst_row) + 2) = m_GDAL.bluedata[ibufpos];
				*(image->data(dst_col, dst_row) + 3) = 255;
				++ibufpos;
				++dst_col;
			}
			++dst_row;
		}
		image->flipVertical();
		return image.release();
	}
	else
		return NULL;
}

osg::HeightField* CDB_Tile::HeightField_From_Tile(void)
{
	if (m_Tile_Status == Loaded)
	{
		osg::ref_ptr<osg::HeightField> field = new osg::HeightField;
		field->allocate(m_Pixels.pixX, m_Pixels.pixY);
		//For now clear the data
		for (unsigned int i = 0; i < field->getHeightList().size(); ++i)
			field->getHeightList()[i] = NO_DATA_VALUE;

		int ibpos = 0;
		for (unsigned int r = 0; r < (unsigned)m_Pixels.pixY; r++)
		{
			unsigned inv_r = m_Pixels.pixY - r - 1;
			for (unsigned int c = 0; c < (unsigned)m_Pixels.pixX; c++)
			{
				//Re-enable this if data is suspect
				//Should already be filtered in CDB creation however
#if 0
				float h = elevation[ibpos];
				// Mark the value as nodata using the universal NO_DATA_VALUE marker.
				if (!isValidValue(h, band))
				{
					h = NO_DATA_VALUE;
				}
#endif
				field->setHeight(c, inv_r, m_GDAL.elevationdata[ibpos]);
				++ibpos;
			}
		}
		return field.release();
	}
	else
		return NULL;
}
