/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
*/

//
// OpenFlight� loader for OpenSceneGraph
//
//  Copyright (C) 2005-2007  Brede Johansen
//

#include "Document.h"

using namespace flt;


Document::Document() :
    _replaceClampWithClampToEdge(false),
    _preserveFace(false),
    _preserveObject(false),
    _replaceDoubleSidedPolys(false),
    _defaultDOFAnimationState(false),
    _useTextureAlphaForTransparancyBinning(true),
    _useBillboardCenter(false),
    _doUnitsConversion(true),
    _readObjectRecordData(false),
    _preserveNonOsgAttrsAsUserData(false),
    _desiredUnits(METERS),
    _done(false),
    _level(0),
    _subfaceLevel(0),
    _unitScale(1.0),
    _version(0),
    _colorPoolParent(false),
    _texturePoolParent(false),
    _materialPoolParent(false),
    _lightSourcePoolParent(false),
    _lightPointAppearancePoolParent(false),
    _lightPointAnimationPoolParent(false),
    _shaderPoolParent(false),
	_textureInarchive(false),
	_remap2Directory(false),
	_Archive(NULL),
	_Archive_FileName(""),
	_Archive_KeyName(""),
	_TextureRemapDirectory("")
{
    _subsurfaceDepth = new osg::Depth(osg::Depth::LESS, 0.0, 1.0,false);
}

Document::~Document()
{
}

void Document::pushLevel()
{
    _levelStack.push_back(_currentPrimaryRecord.get());
    _level++;
}

void Document::popLevel()
{
    _levelStack.pop_back();

    if (!_levelStack.empty())
        _currentPrimaryRecord = _levelStack.back();

    if (--_level<=0)
        _done = true;
}

void Document::pushSubface()
{
    _subfaceLevel++;
}

void Document::popSubface()
{
    _subfaceLevel--;
}

void Document::pushExtension()
{
    if (!_currentPrimaryRecord.valid())
    {
        OSG_WARN << "No current primary in Document::pushExtension()." << std::endl;
        return;
    }

    _extensionStack.push_back(_currentPrimaryRecord.get());
}

void Document::popExtension()
{
    _currentPrimaryRecord=_extensionStack.back().get();
    if (!_currentPrimaryRecord.valid())
    {
        OSG_WARN << "Can't decide primary in Document::popExtension()." << std::endl;
        return;
    }

    _extensionStack.pop_back();
}

osg::Node* Document::getInstanceDefinition(int no)
{
    InstanceDefinitionMap::iterator itr = _instanceDefinitionMap.find(no);
    if (itr != _instanceDefinitionMap.end())
        return (*itr).second.get();

    return NULL;
}

void Document::setSubSurfacePolygonOffset(int level, osg::PolygonOffset* po)
{
    _subsurfacePolygonOffsets[level] = po;
}

osg::PolygonOffset* Document::getSubSurfacePolygonOffset(int level)
{
    OSG_DEBUG<<"Document::getSubSurfacePolygonOffset("<<level<<")"<<std::endl;
    osg::ref_ptr<osg::PolygonOffset>& po = _subsurfacePolygonOffsets[level];
    if (!po)
    {
        po = new osg::PolygonOffset(-1.0f*float(level), -1.0f);
    }
    return po.get();
}

bool Document::OpenArchive(std::string ArchiveName)
{
	_Archive = osgDB::openArchive(ArchiveName, osgDB::ReaderWriter::READ);
	if (_Archive)
	{
		_Archive_FileName = ArchiveName;
		unsigned int pos = _Archive_FileName.rfind(".");
		if (pos != std::string::npos)
		{
			_Archive_KeyName = _Archive_FileName.substr(0, pos);
			unsigned int pos2 = _Archive_KeyName.rfind("\\");
			if ((pos2 != std::string::npos) && (pos2 + 1 < _Archive_KeyName.length()))
			{
				_Archive_KeyName = _Archive_KeyName.substr(pos2 + 1);
			}
		}
		else
			_Archive_KeyName = _Archive_FileName;
		_Archive->getFileNames(_Archive_FileList);
		return true;
	}
	return false;
}

bool Document::SetTexture2MapDirectory(std::string DirectoryName, std::string ModelName)
{
	_TextureRemapDirectory = DirectoryName;
	unsigned int pos = ModelName.rfind("\\");
	bool ret = false;
	if ((pos != std::string::npos) && (pos + 1 < ModelName.length()))
	{
		_Archive_KeyName = ModelName.substr(pos+1);
		unsigned int pos2 = _Archive_KeyName.find("_R");
		if ((pos2 != std::string::npos) && (pos2 + 1 < _Archive_KeyName.length()))
		{
			unsigned int pos3 = _Archive_KeyName.substr(pos2 + 1).find("_");
			if (pos3 != std::string::npos)
			{
				_Archive_KeyName = _Archive_KeyName.substr(0,pos2+pos3+1);
				unsigned int pos4 = _Archive_KeyName.find("D300");
				if (pos4 != std::string::npos)
				{
					_Archive_KeyName.replace(pos4, 4, "D301");
					ret = true;
				}
			}
		}
	}
	else
		_Archive_KeyName = _Archive_FileName;
	return ret;
}

bool Document::MapTextureName2Directory(std::string &textureName)
{
	if (!_TextureRemapDirectory.empty())
	{
		std::string workingname = textureName;
		unsigned int fpos = workingname.rfind("\\");
		if ((fpos != std::string::npos) && (fpos+1 < workingname.length()))
			workingname = workingname.substr(fpos + 1);

		unsigned int len = workingname.length();
		unsigned int pos = workingname.find("_R");
		if (pos == std::string::npos || (pos + 1 >= len))
			return false;
		unsigned int pos2 = workingname.substr(pos + 1).find("_");
		if ((pos2 == std::string::npos) || (pos + pos2 + 1 >= len))
			return false;
		std::string base = workingname.substr(pos + pos2 + 1);
		std::string mappedname = _TextureRemapDirectory;
		mappedname.append("\\");
		mappedname.append(_Archive_KeyName);
		mappedname.append(base);
		textureName = mappedname;
		if (osgDB::fileExists(textureName))
			return true;
	}
	return false;
}


bool Document::MapTextureName2Archive(std::string &textureName)
{
	if (_Archive)
	{
		unsigned int len = textureName.length();
		unsigned int pos = textureName.find("_R");
		if (pos == std::string::npos || (pos+1>=len))
			return false;
		unsigned int pos2 = textureName.substr(pos + 1).find("_");
		if ((pos2 == std::string::npos) || (pos + pos2 + 1 >= len))
			return false;
		std::string base = textureName.substr(pos + pos2 + 1);
		std::string mappedname = _Archive_KeyName;
		mappedname.append(base);
		textureName = mappedname;
		return true;
	}
	else
		return false;
}

std::string  Document::archive_findDataFile(std::string &filename)
{
	std::string result = "";
	for (osgDB::Archive::FileNameList::const_iterator f = _Archive_FileList.begin(); f != _Archive_FileList.end(); ++f)
	{
		const std::string comp = *f;
		if (comp.find(filename) != std::string::npos)
		{
			return comp;
		}
	}
	OSG_WARN <<  "Texture File " << filename << " not found in archive" << std::endl;
	return result;
}

osg::ref_ptr<osg::Image> Document::readArchiveImage(const std::string filename)
{
	if (_Archive)
	{
		osgDB::ReaderWriter::ReadResult r = _Archive->readImage(filename, getOptions());
		if (r.validImage())
			return r.getImage();
	}
	return NULL;
}

void Document::archiveRelease(void)
{
	if (_Archive)
	{
		_Archive.release();
	}
}

double flt::unitsToMeters(CoordUnits unit)
{
    switch (unit)
    {
    case METERS:
        return 1.0;
    case KILOMETERS:
        return 1000.0;
    case FEET:
        return 0.3048;
    case INCHES:
        return 0.02540;
    case NAUTICAL_MILES:
        return 1852.0;
    }

    return 1.0;
}
