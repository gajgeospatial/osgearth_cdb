INCLUDE_DIRECTORIES( ${GDAL_INCLUDE_DIR} )

SET(TARGET_COMMON_LIBRARIES ${TARGET_COMMON_LIBRARIES})

SET(TARGET_SRC
    CDB_Tile.cpp
	CDBTileSource.cpp
	CDBTileSourceDriver.cpp
)

SET(TARGET_H
    CDB_Tile
	CDBTileSource
	CDBTileSourceDriver
)

SET(TARGET_LIBRARIES_VARS GDAL_LIBRARY )

SETUP_PLUGIN(osgearth_cdb)


# to install public driver includes:
SET(LIB_NAME cdb)
SET(LIB_PUBLIC_HEADERS CDBOptions)
INCLUDE(ModuleInstallOsgEarthDriverIncludes OPTIONAL)

