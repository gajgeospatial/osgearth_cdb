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
#include <CDB_TileLib/CDB_Tile>

#include <osgEarth/Version>
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

#ifdef _MSC_VER
#if _MSC_VER < 1800
#define round osg::round
#endif
#endif

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

struct CDBUnReffedFeatureEntryData {
	int CDBLod;
	std::string ModelZipName;
	std::string TextureZipName;
	std::string ArchiveFileName;
};

typedef CDBFeatureEntryData CDBFeatureEntry;
typedef CDBUnReffedFeatureEntryData CDBUnrefEntry;
typedef std::vector<CDBFeatureEntry> CDBFeatureEntryVec;
typedef std::vector<CDBUnrefEntry> CDBUnrefEntryVec;
typedef std::map<std::string, CDBFeatureEntryVec> CDBEntryMap;
typedef std::map<std::string, CDBUnrefEntryVec> CDBUnrefEntryMap;

static CDBEntryMap				_CDBInstances;
static CDBUnrefEntryMap			_CDBUnReffedInstances;

static __int64 _s_CDB_FeatureID = 0;
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
	  _CDB_GS_uses_GTtex(false),
	  _CDB_No_Second_Ref(true),
	  _CDB_Edit_Support(false),
	  _cur_Feature_Cnt(0),
	  _rootString(""),
	  _cacheDir(""),
	  _dataSet("_S001_T001_")
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
		if (_options.Edit_Support().isSet())
			_CDB_Edit_Support = _options.Edit_Support().value();
		if (_options.No_Second_Ref().isSet())
			_CDB_No_Second_Ref = _options.No_Second_Ref().value();
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

		bool errorset = false;
		std::string Errormsg = "";

		//Find a jpeg2000 driver for the image layer.
		if (!CDB_Tile::Initialize_Tile_Drivers(Errormsg))
		{
			errorset = true;
		}

		return result;
    }

	



    FeatureCursor* createFeatureCursor( const Symbology::Query& query )
    {
        FeatureCursor* result = 0L;
		_cur_Feature_Cnt = 0;
		// Make sure the root directory is set
		if (!_options.rootDir().isSet())
		{
			OE_WARN << "CDB root directory not set!" << std::endl;
			return result;
		}
		const osgEarth::TileKey key = query.tileKey().get();
		const GeoExtent key_extent = key.getExtent();
		CDB_Tile_Type tiletype;
		if (_CDB_geoTypical)
			tiletype = GeoTypicalModel;
		else
			tiletype = GeoSpecificModel;
		CDB_Tile_Extent tileExtent(key_extent.north(), key_extent.south(), key_extent.east(), key_extent.west());

		CDB_Tile *mainTile = new CDB_Tile(_rootString, _cacheDir, tiletype, _dataSet, &tileExtent);

		int Files2check = mainTile->Model_Sel_Count();
		int FilesChecked = 0;
		bool dataOK = false;

		FeatureList features;
		while (FilesChecked < Files2check)
		{
			bool have_file = mainTile->Init_Model_Tile(FilesChecked);
			std::string base = mainTile->FileName(FilesChecked);


			OE_DEBUG << query.tileKey().get().str() << "=" << base << std::endl;

			// check the blacklist:
			if (Registry::instance()->isBlacklisted(base))
				continue;

			if (!have_file)
			{
				Registry::instance()->blacklist(base);
			}

			if (have_file)
			{
				bool fileOk = getFeatures(mainTile, base, features, FilesChecked);
				if (fileOk)
				{
					OE_INFO << LC << "Features " << features.size() << base << std::endl;
				}

				if (fileOk)
					dataOK = true;
				else
					Registry::instance()->blacklist(base);
			}
			++FilesChecked;
		}

		delete mainTile;

		result = dataOK ? new FeatureListCursor( features ) : 0L;

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


	bool getFeatures(CDB_Tile *mainTile, const std::string& buffer, FeatureList& features, int sel)
	{
		// find the right driver for the given mime type
		OGR_SCOPED_LOCK;
		// find the right driver for the given mime type
		bool have_archive = false;
		bool have_texture_zipfile = false;

		std::string TileNameStr;
		if (_CDB_Edit_Support)
		{
			TileNameStr = osgDB::getSimpleFileName(buffer);
			TileNameStr = osgDB::getNameLessExtension(TileNameStr);
		}

		const SpatialReference* srs = SpatialReference::create("EPSG:4326");

		osg::ref_ptr<osgDB::Options> localoptions = _dbOptions->cloneOptions();
		std::string ModelTextureDir = "";
		std::string ModelZipFile = "";
		std::string TextureZipFile = "";
		std::string ModelZipDir = "";
		if (_CDB_inflated)
		{
			if (!_CDB_geoTypical)
			{
				if (!mainTile->Model_Texture_Directory(ModelTextureDir))
					return false;
			}
		}
		else
		{
			if (!_CDB_geoTypical)
			{
				have_archive = mainTile->Model_Geometry_Name(ModelZipFile);
				if (!have_archive)
					return false;
				have_texture_zipfile = mainTile->Model_Texture_Archive(TextureZipFile);
			}
		}
		if (_CDB_GS_uses_GTtex)
			ModelZipDir = mainTile->Model_ZipDir();

		bool done = false;
		while (!done)
		{
			OGRFeatureH feat_handle;
			std::string FullModelName;
			std::string ArchiveFileName;
			std::string ModelKeyName;
			bool Model_in_Archive = false;
			bool valid_model = true;
			feat_handle = (OGRFeatureH)mainTile->Next_Valid_Feature(sel, _CDB_inflated, ModelKeyName, FullModelName, ArchiveFileName, Model_in_Archive);
			if (feat_handle == NULL)
			{
				done = true;
				break;
			}
			if (!Model_in_Archive)
				valid_model = false;

#if OSGEARTH_VERSION_GREATER_OR_EQUAL (2,7,0)
			osg::ref_ptr<Feature> f = OgrUtils::createFeature(feat_handle, getFeatureProfile());
#else
			osg::ref_ptr<Feature> f = OgrUtils::createFeature(feat_handle, srs);
#endif
			f->setFID(_s_CDB_FeatureID);
			++_s_CDB_FeatureID;

			f->set("osge_basename", ModelKeyName);

			if (_CDB_Edit_Support)
			{
				std::stringstream format_stream;
				format_stream << TileNameStr << "_" << std::setfill('0')
					<< std::setw(5) << abs(_cur_Feature_Cnt);

				f->set("name", ModelKeyName);
				std::string transformName = "xform_" + format_stream.str();
				f->set("transformname", transformName);
				std::string mtypevalue;
				if (_CDB_geoTypical)
					mtypevalue = "geotypical";
				else
					mtypevalue = "geospecific";
				f->set("modeltype", mtypevalue);
				f->set("tilename", buffer);
				f->set("selection", sel);
			}
			++_cur_Feature_Cnt;
			if (!_CDB_inflated)
			{
				f->set("osge_modelzip", ModelZipFile);
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
					{
						if(have_texture_zipfile)
							f->set("osge_texturezip", TextureZipFile);
					}
					else
						f->set("osge_gs_uses_gt", ModelZipDir);
				}
				else
				{
					//GeoTypical or CDB database in development path
					f->set("osge_modelname", FullModelName);
					if (!_CDB_geoTypical)
						f->set("osge_modeltexture", ModelTextureDir);
				}
#ifdef _DEBUG
				OE_DEBUG << LC << "Model File " << FullModelName << " Set to Load" << std::endl;
#endif
			}
			else
			{
				if (!_CDB_geoTypical)
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
							if (vi->CDBLod <= _CDBLodNum)
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
							if(_CDB_No_Second_Ref)
								valid_model = false;
#ifdef _DEBUG
							OE_DEBUG << LC << "Model File " << FullModelName << " referenced" << std::endl;
#endif
						}
						else
						{
							OE_INFO << LC << "No Instance of " << ModelKeyName << " found to reference" << std::endl;
							valid_model = false;
						}
					}
					else
					{
						if (have_archive)
						{
							//now check and see if it is an unrefernced model from a lower LOD
							CDBUnrefEntryMap::iterator ui = _CDBUnReffedInstances.find(ModelKeyName);
							if (ui != _CDBUnReffedInstances.end())
							{
								//ok we found it here
								CDBUnrefEntryVec CurentCDBUnRefMap = _CDBUnReffedInstances[ModelKeyName];
								bool have_lod = false;
								CDBUnrefEntryVec::iterator ci;
								int mind = 1000;
								for (CDBUnrefEntryVec::iterator vi = CurentCDBUnRefMap.begin(); vi != CurentCDBUnRefMap.end(); ++vi)
								{
									if (vi->CDBLod <= _CDBLodNum)
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
									//Set the attribution for osgearth to load the previously unreference model
									//Normal CDB path
									valid_model = true;
									ModelZipFile = ci->ModelZipName;
									//A little paranoid verification
									if (!validate_name(ModelZipFile))
									{
										valid_model = false;
									}
									else
										f->set("osge_modelzip", ModelZipFile);
									ArchiveFileName = ci->ArchiveFileName;
									f->set("osge_modelname", ArchiveFileName);

									if (!_CDB_GS_uses_GTtex)
									{
										TextureZipFile = ci->TextureZipName;
										have_texture_zipfile = true;
										if (!TextureZipFile.empty())
										{
											if (!validate_name(TextureZipFile))
												have_texture_zipfile = false;
											else
												f->set("osge_texturezip", TextureZipFile);
										}
										else
											have_texture_zipfile = false;
									}
									else
										f->set("osge_gs_uses_gt", ModelZipDir);
#ifdef _DEBUG
									OE_DEBUG << LC << "Previously unrefferenced Model File " << ci->ArchiveFileName << " set to load" << std::endl;
#endif
									//Ok the model is now set to load and will be added to the referenced list 
									//lets remove it from the unreferenced list
									CurentCDBUnRefMap.erase(ci);
									if (CurentCDBUnRefMap.size() == 0)
									{
										_CDBUnReffedInstances.erase(ui);
									}
								}
								else
								{
									OE_INFO << LC << "No Instance of " << ModelKeyName << " found to reference" << std::endl;
								}

							}
							else
							{
								//Its toast and will be a red flag in the database
								OE_INFO << LC << "Model File " << FullModelName << " not found in archive" << std::endl;
							}
						}
						else
						{
							//Its toast and will be a red flag in the database
							OE_INFO << LC << " GeoTypical Model File " << FullModelName << " not found " << std::endl;
						}
					}
				}
				else
				{
					//This is a GeoTypical Model
					if (_CDB_No_Second_Ref)
					{
						if (f->hasAttr("inst"))
						{
							int instanceType = f->getInt("inst");
							if (instanceType == 1)
							{
								valid_model = false;
							}
						}

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
//test
				if (valid_model)
					features.push_back(f.release());
				else
					f.release();
			}
			OGR_F_Destroy(feat_handle);
		}
		if (have_archive)
		{
			//Verify all models in the archive have been referenced
			//If not store them in unreferenced
			std::string Header = mainTile->Model_HeaderName();
			osgDB::Archive::FileNameList * archiveFileList = mainTile->Model_Archive_List();

			for (osgDB::Archive::FileNameList::const_iterator f = archiveFileList->begin(); f != archiveFileList->end(); ++f)
			{
				const std::string archiveFileName = *f;
				std::string KeyName = mainTile->Model_KeyNameFromArchiveName(archiveFileName, Header);
				if (!KeyName.empty())
				{
					CDBEntryMap::iterator mi = _CDBInstances.find(KeyName);
					if (mi == _CDBInstances.end())
					{
						//The model is not in our refernced models so add it to the unreferenced list
						//so we can find it later when it is referenced.
						//This really shouldn't happen and perhaps we will make this an optiont to speed things 
						//up in the future but there are unfortunatly publised datasets with this condition
						//Colorodo Springs is and example
						CDBUnrefEntryMap::iterator ui = _CDBUnReffedInstances.find(KeyName);
						CDBUnrefEntry NewCDBUnRefEntry;
						NewCDBUnRefEntry.CDBLod = _CDBLodNum;
						NewCDBUnRefEntry.ArchiveFileName = archiveFileName;
						NewCDBUnRefEntry.ModelZipName = ModelZipFile;
						NewCDBUnRefEntry.TextureZipName = TextureZipFile;
						if (ui == _CDBUnReffedInstances.end())
						{
							CDBUnrefEntryVec NewCDBUnRefEntryMap;
							NewCDBUnRefEntryMap.push_back(NewCDBUnRefEntry);
							_CDBUnReffedInstances.insert(std::pair<std::string, CDBUnrefEntryVec>(KeyName, NewCDBUnRefEntryMap));
						}
						else
						{
							CDBUnrefEntryVec CurentCDBUnRefEntryMap = _CDBUnReffedInstances[KeyName];
							bool can_insert = true;
							for (CDBUnrefEntryVec::iterator vi = CurentCDBUnRefEntryMap.begin(); vi != CurentCDBUnRefEntryMap.end(); ++vi)
							{
								if (vi->CDBLod == _CDBLodNum)
								{
									can_insert = false;
									break;
								}
							}
							if (can_insert)
								_CDBUnReffedInstances[KeyName].push_back(NewCDBUnRefEntry);
						}
					}
				}
			}
		}
		return true;
	}



	bool validate_name(std::string &filename)
	{
#ifdef _WIN32
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
#else
		int ftyp = ::access(filename.c_str(), F_OK);
		if (ftyp == 0)
		{
			return  true;
		}
		else
		{
			return false;
		}
#endif
	}


	const CDBFeatureOptions         _options;
    FeatureSchema                   _schema;
	bool							_CDB_inflated;
	bool							_CDB_geoTypical;
	bool							_CDB_GS_uses_GTtex;
	bool							_CDB_No_Second_Ref;
	bool							_CDB_Edit_Support;
    osg::ref_ptr<CacheBin>          _cacheBin;
    osg::ref_ptr<osgDB::Options>    _dbOptions;
	int								_CDBLodNum;
	std::string						_rootString;
	std::string						_cacheDir;
	std::string						_dataSet;
	int								_cur_Feature_Cnt;
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

