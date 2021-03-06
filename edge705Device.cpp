/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * GarminPlugin
 * Copyright (C) Andreas Diesner 2010 <andreas.diesner [AT] gmx [DOT] de>
 *
 * GarminPlugin is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GarminPlugin is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "gpsDevice.h"
#include <iostream>
#include <fstream>
#include "log.h"
#include <unistd.h>
#include <sys/stat.h>

#include <stdlib.h>
#include "edge705Device.h"
#include <dirent.h>

Edge705Device::Edge705Device()
{
    this->displayName = "Edge705";
    this->fitnessFileExtension = "tcx";
}

Edge705Device::~Edge705Device() {
    Log::dbg("Edge705Device destructor");
}

void Edge705Device::setPathesFromConfiguration() {
    GarminFilebasedDevice::setPathesFromConfiguration();

    this->fitnessDirectory = this->baseDirectory; // Fallback
    // Set fitness directory from configuration
    if (this->deviceDescription != NULL) {
        TiXmlElement * node = this->deviceDescription->FirstChildElement("Device");
        if (node!=NULL) { node = node->FirstChildElement("MassStorageMode"); }
        if (node!=NULL) { node = node->FirstChildElement("DataType"); }
        while ( node != NULL) {
            TiXmlElement * node2 = node->FirstChildElement("Name");
            if (node2 != NULL) {
                string nameText = node2->GetText();
                if (nameText.compare("FitnessHistory") == 0) {
                    node2 = node->FirstChildElement("File");
                    while (node2 != NULL) {

                        TiXmlElement * transferDirection = node2->FirstChildElement("TransferDirection");
                        string transDir = transferDirection->GetText();
                        if ((transDir.compare("OutputFromUnit") == 0) || (transDir.compare("InputOutput") == 0)) {
                            TiXmlElement * loc = NULL;
                            if (node2!=NULL) { loc = node2->FirstChildElement("Location"); }
                            if (loc!=NULL)   { node2 = loc->FirstChildElement("Path"); }
                            if (node2!=NULL) {
                                this->fitnessDirectory = this->baseDirectory + "/" + node2->GetText();
                                Log::dbg("Fitness directory is: "+this->fitnessDirectory);
                            }
                            if (loc!=NULL)   { node2 = loc->FirstChildElement("FileExtension"); }
                            if (node2!=NULL) {
                                this->fitnessFileExtension = node2->GetText();
                                Log::dbg("Setting file extension to "+this->fitnessFileExtension);
                            }
                        }
                        node2 = node2->NextSiblingElement("File");
                    }
                }
            }
            node = node->NextSiblingElement( "DataType" );
        }
    }
}

int Edge705Device::startReadFitnessData()
{
    if (Log::enabledDbg()) Log::dbg("Starting thread to read from garmin device");

    this->workType = READFITNESS;

    if (startThread()) {
        return 1;
    }

    return 0;
}

int Edge705Device::finishReadFitnessData()
{
/*
    0 = idle
    1 = working
    2 = waiting
    3 = finished
*/

    lockVariables();
    int status = this->threadState;
    unlockVariables();

    return status;
}


void Edge705Device::doWork() {
    if (this->workType == WRITEGPX) {
        this->writeGpxFile();
    } else if (this->workType == READFITNESS) {
        this->readFitnessDataFromDevice(true, "");
    } else if (this->workType == READFITNESSDIR) {
        this->readFitnessDataFromDevice(false, "");
    } else if (this->workType == READFITNESSDETAIL) {
        this->readFitnessDataFromDevice(false, this->readFitnessDetailId);
    } else {
        Log::err("Work Type not implemented!");
    }
}


void Edge705Device::readFitnessDataFromDevice(bool readTrackData, string fitnessDetailId) {
    Log::dbg("Thread readFitnessData started");
/*
Thread-Status
    0 = idle
    1 = working
    2 = waiting
    3 = finished
*/
    string workDir;
    lockVariables();
    this->threadState = 1; // Working
    workDir = this->fitnessDirectory;
    unlockVariables();


    DIR *dp;
    struct dirent *dirp;
    vector<string> files = vector<string>();

    if((dp = opendir(workDir.c_str())) == NULL) {
        Log::err("Error opening fitness directory! "+ workDir);

        lockVariables();
        this->fitnessDataTcdXml = "";
        this->threadState = 3; // Finished
        this->transferSuccessful = false; // Failed;
        unlockVariables();
        return;
    }

    while ((dirp = readdir(dp)) != NULL) {
        files.push_back(string(dirp->d_name));
    }
    closedir(dp);

    TiXmlDocument * output = new TiXmlDocument();
    TiXmlDeclaration * decl = new TiXmlDeclaration( "1.0", "UTF-8", "no");
    output->LinkEndChild( decl );

    TiXmlElement * train = new TiXmlElement( "TrainingCenterDatabase" );
    train->SetAttribute("xmlns","http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2");
    train->SetAttribute("xmlns:xsi","http://www.w3.org/2001/XMLSchema-instance");
    train->SetAttribute("xsi:schemaLocation","http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2 http://www.garmin.com/xmlschemas/TrainingCenterDatabasev2.xsd");

    output->LinkEndChild( train );

    TiXmlElement * activities = new TiXmlElement( "Activities" );
    train->LinkEndChild( activities );

    // Loop over all files in Fitnessdirectory:
    for (unsigned int i = 0;i < files.size();i++) {
        if (files[i].find("."+this->fitnessFileExtension)!=string::npos) {
            TiXmlDocument doc(workDir + "/" + files[i]);
            if (doc.LoadFile()) {
                TiXmlElement * train = doc.FirstChildElement("TrainingCenterDatabase");
                TiXmlElement * inputActivities = train->FirstChildElement("Activities");
                while ( inputActivities != NULL) {
                    TiXmlElement * inputActivity =inputActivities->FirstChildElement("Activity");
                    while ( inputActivity != NULL) {

                        string currentLapId="";
                        TiXmlElement * idNode = inputActivity->FirstChildElement("Id");
                        if (idNode != NULL) { currentLapId = idNode->GetText(); }

                        if ((fitnessDetailId.length() == 0) || (fitnessDetailId.compare(currentLapId) == 0)) {
                            TiXmlNode * newAct = inputActivity->Clone();

                            if (readTrackData) {
                                // Track data must be deleted
                                TiXmlNode * node = newAct->FirstChildElement("Lap");
                                if ((node != NULL) && (node->FirstChildElement("Track") != NULL)) { node->RemoveChild( node->FirstChildElement("Track") );}
                            }

                            activities->LinkEndChild( newAct );

                            if (Log::enabledDbg()) { Log::dbg("Adding activity "+currentLapId+" from file "+files[i]); }
                        }
                        inputActivity = inputActivity->NextSiblingElement( "Activity" );
                    }
                    inputActivities = inputActivities->NextSiblingElement( "Activities" );
                }
            } else {
                Log::err("Unable to load fitness file "+files[i]);
            }
        }
    }

    TiXmlPrinter printer;
    printer.SetIndent( "  " );
    output->Accept( &printer );
    string fitnessXml = printer.Str();
    delete(output);

    lockVariables();
    this->fitnessDataTcdXml = fitnessXml;
    this->threadState = 3; // Finished
    this->transferSuccessful = true; // Successfull;
    unlockVariables();

    if (Log::enabledDbg()) { Log::dbg("Thread readFitnessData finished"); }
    return;
}


string Edge705Device::getFitnessData() {
    return this->fitnessDataTcdXml;
}

int Edge705Device::startReadFITDirectory() {
    if (Log::enabledDbg()) Log::dbg("Starting thread to read from garmin device");

    this->workType = READFITNESSDIR;

    if (startThread()) {
        return 1;
    }

    return 0;
}

int Edge705Device::startReadFitnessDirectory() {
    if (Log::enabledDbg()) Log::dbg("Starting thread to read from garmin device");

    this->workType = READFITNESSDIR;

    if (startThread()) {
        return 1;
    }

    return 0;
}

int Edge705Device::finishReadFitnessDirectory() {
    lockVariables();
    int status = this->threadState;
    unlockVariables();

    return status;
}

void Edge705Device::cancelReadFitnessData() {
}


int Edge705Device::startReadFitnessDetail(string id) {
    if (Log::enabledDbg()) Log::dbg("Starting thread to read fitness detail from garmin device: "+this->displayName+ " Searching for "+id);

    this->workType = READFITNESSDETAIL;
    this->readFitnessDetailId = id;

    if (startThread()) {
        return 1;
    }

    return 0;
}

int Edge705Device::finishReadFitnessDetail() {
    lockVariables();
    int status = this->threadState;
    unlockVariables();

    return status;
}

void Edge705Device::cancelReadFitnessDetail() {
    cancelThread();
}
