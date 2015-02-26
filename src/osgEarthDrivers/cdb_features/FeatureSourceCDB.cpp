/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2013 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
// 2014-2015 GAJ Geospatial Enterprises, Orlando FL
// Created FeatureSourceCDB for Incorporation of Common Database (CDB) support within osgEarth

#include "CDBFeatureOptions"

#include <osgEarth/Registry>
#include <osgEarth/XmlUtils>
#include <osgEarth/FileUtils>
#include <osgEarthFeatures/FeatureSource>
#include <osgEarthFeatures/Filter>
#include <osgEarthFeatures/BufferFilter>
#include <osgEarthFeatures/ScaleFilter>
#include <osgEarthFeatures/OgrUtils>
#include <osgEarthUtil/TFS>
#include <osg/Notify>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Archive>
#include <list>
#include <vector>
#include <stdio.h>
#include <stdlib.h>

#include <ogr_api.h>
#include <ogr_core.h>
#include <ogrsf_frmts.h>

#ifdef WIN32
#include <windows.h>
#endif

#define LC "[CDB FeatureSource] "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Features;
using namespace osgEarth::Drivers;

#define OGR_SCOPED_LOCK GDAL_SCOPED_LOCK

struct CDBFeatureEntryData {
	int	CDBLod;
	std::string FullReferenceName;
};

typedef CDBFeatureEntryData CDBFeatureEntry;
typedef std::vector<CDBFeatureEntry> CDBFeatureEntryVec;
typedef std::map<std::string, CDBFeatureEntryVec> CDBEntryMap;

static CDBEntryMap				_CDBInstances;

/**
 * A FeatureSource that reads Common Database Layers
 * 
 */
class CDBFeatureSource : public FeatureSource
{
public:
    CDBFeatureSource(const CDBFeatureOptions& options ) :
      FeatureSource( options ),
      _options     ( options ),
	  _CDB_inflated (false),
	  _CDB_geoTypical(false),
	  _CDB_GS_uses_GTtex(false)
    {                
    }

    /** Destruct the object, cleaning up and OGR handles. */
    virtual ~CDBFeatureSource()
    {               
        //nop
    }

    //override
    void initialize( const osgDB::Options* dbOptions )
    {
        _dbOptions = dbOptions ? osg::clone(dbOptions) : 0L;

		osgEarth::CachePolicy::NO_CACHE.apply(_dbOptions.get());
		//ToDo when working reenable  the cache disable for development 

    }


    /** Called once at startup to create the profile for this feature set. Successful profile
        creation implies that the datasource opened succesfully. */
    const FeatureProfile* createFeatureProfile()
    {
        FeatureProfile* result = NULL;
		const Profile * CDBFeatureProfile = NULL;
		if (_options.inflated().isSet())
			_CDB_inflated = _options.inflated().value();
		if (_options.GS_uses_GTtex().isSet())
			_CDB_GS_uses_GTtex = _options.GS_uses_GTtex().value();
		if (_options.geoTypical().isSet())
		{
			_CDB_geoTypical = _options.geoTypical().value();
			if (_CDB_geoTypical)
			{
				if (!_CDB_inflated)
				{
					OE_WARN << "GeoTypical option was set without CDB_Inflated: Forcing Inflated" << std::endl;
					_CDB_inflated = true;
				}
			}
		}

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
				if ((max_lon > min_lon) && (max_lat > min_lat))
				{
					unsigned tiles_x = (unsigned)(max_lon - min_lon);
					unsigned tiles_y = (unsigned)(max_lat - min_lat);
					osg::ref_ptr<const SpatialReference> src_srs;
					src_srs = SpatialReference::create("EPSG:4326");
					CDBFeatureProfile = osgEarth::Profile::create(src_srs, min_lon, min_lat, max_lon, max_lat, tiles_x, tiles_y);

					//			   Below works but same as no limits
					//			   setProfile(osgEarth::Profile::create(src_srs, -180.0, -90.0, 180.0, 90.0, min_lon, min_lat, max_lon, max_lat, 90U, 45U));
				}
			}
			if (!CDBFeatureProfile)
				OE_WARN << "Invalid Limits received by CDB Driver: Not using Limits" << std::endl;

		}
		int minLod, maxLod;
		if (_options.minLod().isSet())
			minLod = _options.minLod().value();
		else
			minLod = 2;

		if (_options.maxLod().isSet())
		{
			maxLod = _options.maxLod().value();
			if (maxLod < minLod)
				minLod = maxLod;
		}
		else
			maxLod = minLod;

		// Always a WGS84 unprojected lat/lon profile.
		if (!CDBFeatureProfile)
			CDBFeatureProfile =osgEarth::Profile::create("EPSG:4326", "", 90U, 45U);

		result = new FeatureProfile(CDBFeatureProfile->getExtent());
		result->setTiled( true );
		// Should work for now 
		result->setFirstLevel(minLod);
		result->setMaxLevel( maxLod);
		result->setProfile(CDBFeatureProfile);

		// Make sure the root directory is set
		if (!_options.rootDir().isSet())
		{
			OE_WARN << "CDB root directory not set!" << std::endl;
		}
		else
		{
			_rootString = _options.rootDir().value();
		}


		return result;
    }

	



    FeatureCursor* createFeatureCursor( const Symbology::Query& query )
    {
        FeatureCursor* result = 0L;

		// Make sure the root directory is set
		if (!_options.rootDir().isSet())
		{
			OE_WARN << "CDB root directory not set!" << std::endl;
			return result;
		}

		GetPathComponents(query.tileKey().get());


		std::string base = Base_Shapefile_Name();


		OE_DEBUG << query.tileKey().get().str() << "=" << base << std::endl;

		// check the blacklist:
		if (Registry::instance()->isBlacklisted(base))
			return result;


		DWORD ftyp = ::GetFileAttributes(base.c_str());
		if(ftyp == INVALID_FILE_ATTRIBUTES)
		{
			DWORD error = ::GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			{
				Registry::instance()->blacklist(base);
				return result;
			}
		}

		bool dataOK = false;

		FeatureList features;
		dataOK = getFeatures( base, features );
		

		if ( dataOK )
		{
			OE_INFO << LC << "Features " << features.size() << base <<  std::endl;
		}

		result = dataOK ? new FeatureListCursor( features ) : 0L;

		if ( !result )
			Registry::instance()->blacklist(base);



        return result;
    }

    /**
    * Gets the Feature with the given FID
    * @returns
    *     The Feature with the given FID or NULL if not found.
    */
    virtual Feature* getFeature( FeatureID fid )
    {
        return 0;
    }

    virtual bool isWritable() const
    {
        return false;
    }

    virtual const FeatureSchema& getSchema() const
    {
        //TODO:  Populate the schema from the DescribeFeatureType call
        return _schema;
    }

    virtual osgEarth::Symbology::Geometry::Type getGeometryType() const
    {
        return Geometry::TYPE_UNKNOWN;
    }

private:

#ifdef _MSC_VER
#if _MSC_VER < 1800
	double round(double x)
	{
		return (double)((int)(x + 0.4999999999));
	}
#endif
#endif

	bool getFeatures(const std::string& buffer, FeatureList& features)
	{
		// find the right driver for the given mime type
		OGR_SCOPED_LOCK;
		// find the right driver for the given mime type
		OGRSFDriverH ogrDriver = OGRGetDriverByName("ESRI Shapefile");
		OGRSFDriver *dbfDriver = OGRSFDriverRegistrar::GetRegistrar()->GetDriverByName("ESRI Shapefile");
		// fail if we can't find an appropriate OGR driver:
		if (!ogrDriver)
		{
			OE_WARN << LC << "Error obtaining Shapefile OGR Driver"
				<< std::endl;
			return false;
		}

		//Open the shapefile
		OGRDataSourceH ds = OGROpen(buffer.c_str(), FALSE, &ogrDriver);

		//Open the secondary attributes file
		std::string dbf = Secondary_Dbf_Name(".dbf");
		OGRDataSource *dbfds = dbfDriver->Open(dbf.c_str(), FALSE);
		if (!dbfds)
		{
			//Check for junk files clogging up the works
			std::string shx = Secondary_Dbf_Name(".shx");
			DWORD ftyp = ::GetFileAttributes(shx.c_str());
			if (ftyp != INVALID_FILE_ATTRIBUTES)
			{
				if (::DeleteFile(shx.c_str()) == 0)
				{
					OE_INFO << LC << "Error deleteing empty shx file" << std::endl;
				}
			}
			std::string shp = Secondary_Dbf_Name(".shp");
			ftyp = ::GetFileAttributes(shp.c_str());
			if (ftyp != INVALID_FILE_ATTRIBUTES)
			{
				if (::DeleteFile(shp.c_str()) == 0)
				{
					OE_INFO << LC << "Error deleteing empty shp file" << std::endl;
				}
			}
			dbfds = dbfDriver->Open(dbf.c_str(), FALSE);
		}
		if (!ds)
		{
			OE_WARN << LC << "Error reading CDB response" << std::endl;
			return false;
		}

		if (!dbfds)
		{
			OE_WARN << LC << "Error reading secondary CDB response" << std::endl;
			return false;
		}

		// read the feature data.
		OGRLayerH layer = OGR_DS_GetLayer(ds, 0);
		OGRLayer *dbflayer = dbfds->GetLayer(0);
		osg::ref_ptr<osgDB::Archive> ar = NULL;
		bool have_archive = false;
		if (layer && dbflayer)
		{
			const SpatialReference* srs = SpatialReference::create("EPSG:4326");

			osg::ref_ptr<osgDB::Options> localoptions = _dbOptions->cloneOptions();

			OGR_L_ResetReading(layer);
			dbflayer->ResetReading();

			int name_attr_index = find_dbf_string_field(dbflayer, "MODL");
			if (name_attr_index < 0)
			{
				OE_WARN << LC << "Unable to locate Model Name field in secondary dbf" << std::endl;
				OGR_DS_Destroy(ds);
				OGR_DS_Destroy(dbfds);
				return false;
			}

			int cnam_attr_index = find_dbf_string_field(dbflayer, "CNAM");
			if (cnam_attr_index < 0)
			{
				OE_WARN << LC << "Unable to locate Attribute Key field in secondary dbf" << std::endl;
				OGR_DS_Destroy(ds);
				OGR_DS_Destroy(dbfds);
				return false;
			}

			int facc_index = find_dbf_string_field(dbflayer, "FACC");
			if (facc_index < 0)
			{
				OE_WARN << LC << "Unable to locate FACC field in secondary dbf" << std::endl;
				OGR_DS_Destroy(ds);
				OGR_DS_Destroy(dbfds);
				return false;
			}


			OGRFeatureH feat_handle;
			osgDB::Archive::FileNameList archiveFileList;

			while ((feat_handle = OGR_L_GetNextFeature(layer)) != NULL)
			{
				std::string NameAttrString;
				if (feat_handle)
				{
					osg::ref_ptr<Feature> f = OgrUtils::createFeature(feat_handle, srs);
					if (f->hasAttr("cnam"))
					{
						NameAttrString = f->getString("cnam");
						if (NameAttrString.length() < 8)
						{
							OE_WARN << LC << "Invalid cnam field in primary dbf" << std::endl;
							OGR_F_Destroy(feat_handle);
							break;
						}
					}
					else
					{
						OE_WARN << LC << "Unable to locate cnam field in primary dbf" << std::endl;
						OGR_F_Destroy(feat_handle);
						break;
					}

					std::string BaseFileName;
					std::string FACC_value;
					std::string ModelType;
					std::string FullModelName;
					std::string ArchiveFileName;
					std::string ModelTextureDir;
					std::string ModelZipFile;
					std::string TextureZipFile;
					std::string ModelKeyName;
					bool valid_model = true;

					bool found_key = find_dbf_string_field_by_key(dbflayer, name_attr_index, BaseFileName, cnam_attr_index, NameAttrString,
																  facc_index, FACC_value);
					if (!found_key)
					{
						OE_WARN << LC << "Key " << NameAttrString << " missing in secondary dbf" << std::endl;
						valid_model = false;
					}
					else
					{

						ModelKeyName = Model_KeyName(NameAttrString, BaseFileName);
						f->set("osge_basename", ModelKeyName);

						if (_CDB_inflated)
						{
							//This GeoTypical or a database that is in the process of creation
							//Models and textures have not been zipped into archives
							if (_CDB_geoTypical)
								FullModelName = GeoTypical_FullFileName(ModelKeyName);
							else
								FullModelName = Model_FullFileName(NameAttrString, BaseFileName);

							if (!validate_name(FullModelName))
							{
								OE_DEBUG << LC << "Key " << NameAttrString << "FACC " << FACC_value << std::endl;
								valid_model = false;
							}

							if (!_CDB_geoTypical)
							{
								ModelTextureDir = Model_TextureDir();
								if (!validate_name(ModelTextureDir))
									valid_model = false;
							}
						}
						else
						{
							//Standard CDB models. Models (.flt) are stored in a zipped archives
							//Model textures are stored in a seperate zip archive.
							FullModelName = Model_FileName(NameAttrString, BaseFileName);

							ModelZipFile = Model_ZipName();
							if (!validate_name(ModelZipFile))
								valid_model = false;
							else
								f->set("osge_modelzip", ModelZipFile);

							if (!_CDB_GS_uses_GTtex)
							{
								TextureZipFile = Model_TextureZip();
								if (!validate_name(TextureZipFile))
									valid_model = false;
							}

							if (valid_model && !have_archive)
							{
								//Our archive file exists however we have not opened it yet. Open it now.
								ar = osgDB::openArchive(ModelZipFile, osgDB::ReaderWriter::ArchiveStatus::READ);
								if (ar)
								{
									//Get the list of files that are in the archive.
									ar->getFileNames(archiveFileList);
									have_archive = true;
								}
							}

							if (have_archive)
							{
								//Verify that the model is in the archive. If it is not in the archive it should have been
								//loaded in a previous lod
								ArchiveFileName = archive_validate_name(archiveFileList, FullModelName);
								if (ArchiveFileName.empty())
								{
									valid_model = false;
								}
							}
							else
								valid_model = false;
						}//end else !cdb_inflated

						if (valid_model)
						{
							//Ok we have everthing needed to load this model at this lod
							//Set the atribution to tell osgearth to load the model
							if (have_archive)
							{
								//Normal CDB path
								f->set("osge_modelname", ArchiveFileName);
								if (!_CDB_GS_uses_GTtex)
									f->set("osge_texturezip", TextureZipFile);
								else
									f->set("osge_gs_uses_gt", Model_ZipDir());
							}
							else
							{
								//GeoTypical or CDB database in development path
								f->set("osge_modelname", FullModelName);
								if (!_CDB_geoTypical)
									f->set("osge_modeltexture", ModelTextureDir);
							}
						}
						else
						{
							//The model does not exist at this lod. It should have been loaded previously
							//Look up the exact name used when creating the model at the lower lod
							CDBEntryMap::iterator mi = _CDBInstances.find(ModelKeyName);
							if (mi != _CDBInstances.end())
							{
								//Ok we found the model select the best lod. It must be lower than our current lod
								//If the model is not found here then we will simply ignore the model until we get to an lod in which
								//we find the model. If we selected to start at an lod higher than 0 there will be quite a few models
								//that fall into this catagory
								CDBFeatureEntryVec CurentCDBEntryMap = _CDBInstances[ModelKeyName];
								bool have_lod = false;
								CDBFeatureEntryVec::iterator ci;
								int mind = 1000;
								for (CDBFeatureEntryVec::iterator vi = CurentCDBEntryMap.begin(); vi != CurentCDBEntryMap.end(); ++vi)
								{
									if (vi->CDBLod < _CDBLodNum)
									{
										int cind = _CDBLodNum - vi->CDBLod;
										if (cind < mind)
										{
											have_lod = true;
											ci = vi;
										}
									}
								}
								if (have_lod)
								{
									//Set the attribution for osgearth to find the referenced model
									std::string referencedName = ci->FullReferenceName;
									f->set("osge_modelname", referencedName);
								}
							}
							else
							{
								OE_INFO << LC << "Model File " << FullModelName << " not found in archive" << std::endl;
								OE_INFO << LC << "Key " << NameAttrString << "FACC " << FACC_value << std::endl;
							}

						}
					}
					if (f.valid() && !isBlacklisted(f->getFID()))
					{
						if (valid_model && !_CDB_geoTypical)
						{
							//We need to record this instance so that this model reference can be found when referenced in 
							//higher lods. In order for osgearth to find the model we must have the exact model name that was used
							//in either a filename or archive reference
							CDBEntryMap::iterator mi = _CDBInstances.find(ModelKeyName);
							CDBFeatureEntry NewCDBEntry;
							NewCDBEntry.CDBLod = _CDBLodNum;
							if (have_archive)
								NewCDBEntry.FullReferenceName = ArchiveFileName;
							else
								NewCDBEntry.FullReferenceName = FullModelName;

							if (mi == _CDBInstances.end())
							{
								CDBFeatureEntryVec NewCDBEntryMap;
								NewCDBEntryMap.push_back(NewCDBEntry);
								_CDBInstances.insert(std::pair<std::string, CDBFeatureEntryVec>(ModelKeyName, NewCDBEntryMap));
							}
							else
							{
								CDBFeatureEntryVec CurentCDBEntryMap = _CDBInstances[ModelKeyName];
								bool can_insert = true;
								for (CDBFeatureEntryVec::iterator vi = CurentCDBEntryMap.begin(); vi != CurentCDBEntryMap.end(); ++vi)
								{
									if (vi->CDBLod == _CDBLodNum)
									{
										can_insert = false;
										break;
									}
								}
								if (can_insert)
									_CDBInstances[ModelKeyName].push_back(NewCDBEntry);
							}
						}
						features.push_back(f.release());
					}
					OGR_F_Destroy(feat_handle);
				}
			}
		}
		if (have_archive)
			ar.release();
		// Destroy the datasource
		OGR_DS_Destroy(ds);
		OGR_DS_Destroy(dbfds);
		return true;
	}


	void GetPathComponents(const osgEarth::TileKey& key)
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


		int londir = (int)lont;
		std::stringstream format_stream_1;
		format_stream_1 << ((londir < 0) ? "W" : "E") << std::setfill('0')
			<< std::setw(3) << abs(londir);
		_lon_string = format_stream_1.str();


		double latt = (double)((int)key_extent.south());
		//make sure there wasn't a rounding error
		if (abs((latt + 1.0) - key_extent.south()) < DBL_EPSILON)
			latt += 1.0;
		else if (key_extent.south() < latt)//We're in the Southern Hemisphere round down.
			latt -= 1.0;

		int latdir = (int)latt;
		std::stringstream format_stream_2;
		format_stream_2 << ((latdir < 0) ? "S" : "N") << std::setfill('0')
			<< std::setw(2) << abs(latdir);
		_lat_string = format_stream_2.str();

		// Set the LOD of the request
		std::stringstream lod_stream;
		if (cdbLod < 0)
			lod_stream << "LC" << std::setfill('0') << std::setw(2) << abs(cdbLod);
		else
			lod_stream << "L" << std::setfill('0') << std::setw(2) << cdbLod;
		_lod_string = lod_stream.str();

		_CDBLodNum = cdbLod;
		if (cdbLod < 1)
		{
			//There is only one tile in cdb levels 0 and below
			//
			_uref_string = "U0";
			_rref_string = "R0";
		}
		else
		{
			int u = 1 << cdbLod;
			// Determine UREF
			std::stringstream uref_stream;
			uref_stream << "U" << tile_y;
			_uref_string = uref_stream.str();

			// Determine RREF
			std::stringstream rref_stream;
			rref_stream << "R" << tile_x;
			_rref_string = rref_stream.str();
		}


	}


	int find_dbf_string_field(OGRLayer *poLayer, std::string fieldname)
	{
		OGRFeatureDefn	*poFDefn = poLayer->GetLayerDefn();
		int dbfieldcnt = poFDefn->GetFieldCount();
		for (int dbffieldIdx = 0; dbffieldIdx < dbfieldcnt; ++dbffieldIdx)
		{
			OGRFieldDefn *po_FieldDefn = poFDefn->GetFieldDefn(dbffieldIdx);
			std::string thisname = po_FieldDefn->GetNameRef();
			if (thisname.compare(fieldname) == 0)
			{
				if (po_FieldDefn->GetType() == OFTString)
					return dbffieldIdx;
			}
		}
		return -1;
	}

	bool find_dbf_string_field_by_key(OGRLayer *poLayer, int &field_index, std::string &field_value, int &key_index, std::string &Keyname,
		int &facc_index, std::string &facc_value)
	{
		poLayer->ResetReading();
		OGRFeature* dbf_feature;
		field_value = "";
		while ((dbf_feature = poLayer->GetNextFeature()) != NULL)
		{
			std::string key_check = dbf_feature->GetFieldAsString(key_index);
			if (key_check.compare(Keyname) == 0)
			{
				field_value = dbf_feature->GetFieldAsString(field_index);
				facc_value = dbf_feature->GetFieldAsString(facc_index);

				OGRFeature::DestroyFeature(dbf_feature);
				return true;
			}
			OGRFeature::DestroyFeature(dbf_feature);

		}
		return false;
	}

	int get_dbf_string_field_by_key_count(OGRLayer *poLayer, int &field_index, std::string &Keyname, int &key_index, std::string &LastEntry)
	{
		poLayer->ResetReading();
		OGRFeature* dbf_feature;
		int count = 0;
		while ((dbf_feature = poLayer->GetNextFeature()) != NULL)
		{
			std::string key_check = dbf_feature->GetFieldAsString(key_index);
			if (key_check.compare(Keyname) == 0)
			{
				++count;
				LastEntry = dbf_feature->GetFieldAsString(field_index);
			}
			OGRFeature::DestroyFeature(dbf_feature);

		}
		return count;
	}


	bool validate_name(std::string &filename)
	{
		DWORD ftyp = ::GetFileAttributes(filename.c_str());
		if (ftyp == INVALID_FILE_ATTRIBUTES)
		{
			DWORD error = ::GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			{
				OE_DEBUG << LC << "Model File " << filename << " not found" << std::endl;
				return false;
			}
		}
		return true;
	}

	std::string  archive_validate_name(osgDB::Archive::FileNameList &archiveFileList, std::string &filename)
	{
		std::string result = "";
		for (osgDB::Archive::FileNameList::const_iterator f = archiveFileList.begin(); f != archiveFileList.end(); ++f)
		{
			const std::string comp = *f;
			if (comp.find(filename) != std::string::npos)
			{
				return comp;
			}
		}
		return result;
	}

	std::string Base_Shapefile_Name(void)
	{
		std::stringstream buf;

		if (_CDB_geoTypical)
		{
			buf << _rootString
				<< "\\Tiles"
				<< "\\" << _lat_string
				<< "\\" << _lon_string
				<< "\\101_GTFeature"
				<< "\\" << _lod_string
				<< "\\" << _uref_string
				<< "\\" << _lat_string << _lon_string << "_D101_S002_T001_" << _lod_string
				<< "_" << _uref_string << "_" << _rref_string << ".shp";
		}
		else
		{
			buf << _rootString
				<< "\\Tiles"
				<< "\\" << _lat_string
				<< "\\" << _lon_string
				<< "\\100_GSFeature"
				<< "\\" << _lod_string
				<< "\\" << _uref_string
				<< "\\" << _lat_string << _lon_string << "_D100_S001_T001_" << _lod_string
				<< "_" << _uref_string << "_" << _rref_string << ".shp";
		}
		return buf.str();
	}

	std::string Secondary_Dbf_Name(std::string ext)
	{
		std::stringstream dbfbuf;
		if (_CDB_geoTypical)
		{
			dbfbuf << _rootString
				<< "\\Tiles"
				<< "\\" << _lat_string
				<< "\\" << _lon_string
				<< "\\101_GTFeature"
				<< "\\" << _lod_string
				<< "\\" << _uref_string
				<< "\\" << _lat_string << _lon_string << "_D101_S002_T002_" << _lod_string
				<< "_" << _uref_string << "_" << _rref_string << ext;
		}
		else
		{
			dbfbuf << _rootString
				<< "\\Tiles"
				<< "\\" << _lat_string
				<< "\\" << _lon_string
				<< "\\100_GSFeature"
				<< "\\" << _lod_string
				<< "\\" << _uref_string
				<< "\\" << _lat_string << _lon_string << "_D100_S001_T002_" << _lod_string
				<< "_" << _uref_string << "_" << _rref_string << ext;
		}
		return dbfbuf.str();
	}

	std::string Model_FullFileName(std::string &AttrName, std::string &BaseFileName)
	{
		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\Tiles"
			<< "\\" << _lat_string
			<< "\\" << _lon_string
			<< "\\300_GSModelGeometry"
			<< "\\" << _lod_string
			<< "\\" << _uref_string
			<< "\\" << _lat_string << _lon_string << "_D300_S001_T001_" << _lod_string
			<< "_" << _uref_string << "_" << _rref_string << "_"
			<< AttrName.substr(0,5) << "_" << AttrName.substr(5,3) << "_"
			<< BaseFileName << ".flt";
		return modbuf.str();
	}

	std::string GeoTypical_FullFileName(std::string &BaseFileName)
	{
		std::string Facc1 = BaseFileName.substr(0, 1);
		std::string Facc2 = BaseFileName.substr(1, 1);
		std::string Fcode = BaseFileName.substr(2, 3);

		//First Level subdirectory
		if (Facc1 == "A")
			Facc1 = "A_Culture";
		else if (Facc1 == "E")
			Facc1 = "E_Vegetation";
		else if (Facc1 == "B")
			Facc1 = "B_Hydrography";
		else if (Facc1 == "C")
			Facc1 = "C_Hypsography";
		else if (Facc1 == "D")
			Facc1 = "D_Physiography";
		else if (Facc1 == "F")
			Facc1 = "F_Demarcation";
		else if (Facc1 == "G")
			Facc1 = "G_Aeronautical_Information";
		else if (Facc1 == "I")
			Facc1 = "I_Cadastral";
		else if (Facc1 == "S")
			Facc1 = "S_Special_Use";
		else
			Facc1 = "Z_General";

		//Second Level Directory
		if (Facc2 == "L")
			Facc2 = "L_Misc_Feature";
		else if (Facc2 == "T")
			Facc2 = "T_Comm";
		else if (Facc2 == "C")
			Facc2 = "C_Woodland";
		else if (Facc2 == "K")
			Facc2 = "K_Recreational";

		if (Facc1 == "A_Culture")
		{
			if (Fcode == "050")
				Fcode = "050_Display_Sign";
			else if (Fcode == "110")
				Fcode = "110_Light_Standard";
			else if (Fcode == "030")
				Fcode = "030_Power_Line";
		}
		else if (Facc1 == "E_Vegetation")
		{
			if (Fcode == "030")
				Fcode = "030_Trees";
		}

		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\GTModel\\500_GTModelGeometry"
			<< "\\" << Facc1
			<< "\\" << Facc2
			<< "\\" << Fcode
			<< "\\D500_S001_T001_" << BaseFileName;

		return modbuf.str();
	}

	std::string Model_KeyName(std::string &AttrName, std::string &BaseFileName)
	{
		std::stringstream modbuf;
		modbuf	<< AttrName.substr(0, 5) << "_" << AttrName.substr(5, 3) << "_"
				<< BaseFileName << ".flt";
		return modbuf.str();
	}

	std::string Model_FileName(std::string &AttrName, std::string &BaseFileName)
	{
		std::stringstream modbuf;
		modbuf << _lat_string << _lon_string << "_D300_S001_T001_" << _lod_string
			<< "_" << _uref_string << "_" << _rref_string << "_"
			<< AttrName.substr(0, 5) << "_" << AttrName.substr(5, 3) << "_"
			<< BaseFileName << ".flt";
		return modbuf.str();
	}

	std::string Model_ZipName(void)
	{
		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\Tiles"
			<< "\\" << _lat_string
			<< "\\" << _lon_string
			<< "\\300_GSModelGeometry"
			<< "\\" << _lod_string
			<< "\\" << _uref_string
			<< "\\" << _lat_string << _lon_string << "_D300_S001_T001_" << _lod_string
			<< "_" << _uref_string << "_" << _rref_string << ".zip";
		return modbuf.str();
	}

	std::string Model_ZipDir(void)
	{
		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\Tiles"
			<< "\\" << _lat_string
			<< "\\" << _lon_string
			<< "\\300_GSModelGeometry"
			<< "\\" << _lod_string
			<< "\\" << _uref_string;
		return modbuf.str();
	}

	std::string Model_TextureDir(void)
	{
		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\Tiles"
			<< "\\" << _lat_string
			<< "\\" << _lon_string
			<< "\\301_GSModelTexture"
			<< "\\" << _lod_string
			<< "\\" << _uref_string;
		return modbuf.str();
	}

	std::string Model_TextureZip(void)
	{
		std::stringstream modbuf;
		modbuf << _rootString
			<< "\\Tiles"
			<< "\\" << _lat_string
			<< "\\" << _lon_string
			<< "\\301_GSModelTexture"
			<< "\\" << _lod_string
			<< "\\" << _uref_string
			<< "\\" << _lat_string << _lon_string << "_D301_S001_T001_" << _lod_string
			<< "_" << _uref_string << "_" << _rref_string << ".zip";
		return modbuf.str();
	}

	const CDBFeatureOptions         _options;
    FeatureSchema                   _schema;
	bool							_CDB_inflated;
	bool							_CDB_geoTypical;
	bool							_CDB_GS_uses_GTtex;
    osg::ref_ptr<CacheBin>          _cacheBin;
    osg::ref_ptr<osgDB::Options>    _dbOptions;
	int								_CDBLodNum;
	std::string						_rootString;
	std::string						_lat_string;
	std::string						_lon_string;
	std::string						_uref_string;
	std::string						_rref_string;
	std::string						_lod_string;
};


class CDBFeatureSourceFactory : public FeatureSourceDriver
{
public:
    CDBFeatureSourceFactory()
    {
        supportsExtension( "osgearth_feature_cdb", "CDB feature driver for osgEarth" );
    }

    virtual const char* className()
    {
        return "CDB Feature Reader";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return ReadResult( new CDBFeatureSource( getFeatureSourceOptions(options) ) );
    }
};

REGISTER_OSGPLUGIN(osgearth_feature_cdb, CDBFeatureSourceFactory)

