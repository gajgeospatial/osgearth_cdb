INCLUDE_DIRECTORIES( ${GDAL_INCLUDE_DIR} )

SET(TARGET_SRC FeatureSourceCDB.cpp)
SET(TARGET_H CDBFeatureOptions)
SET(TARGET_COMMON_LIBRARIES ${TARGET_COMMON_LIBRARIES} osgEarthFeatures osgEarthSymbology)

SET(TARGET_LIBRARIES_VARS GDAL_LIBRARY )

SETUP_PLUGIN(osgearth_feature_cdb)


# to install public driver includes:
SET(LIB_NAME feature_cdb)
SET(LIB_PUBLIC_HEADERS CDBFeatureOptions)
INCLUDE(ModuleInstallOsgEarthDriverIncludes OPTIONAL)

