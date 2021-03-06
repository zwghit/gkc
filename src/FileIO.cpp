/*
 * =====================================================================================
 *
 *       Filename: FileIO.cpp
 *
 *    Description: Data Input/Output using HDF-5 initialization routines
 *
 *         Author: Paul P. Hilscher (2010-), 
 *
 *        License: GPLv3+
 * =====================================================================================
 */

#include <string>
#include <sstream>
#include <stddef.h>

#include "FileIO.h"
#include "Plasma.h"
#include "Special/Vector3D.h"
#include "Tools/System.h"

FileIO::FileIO(Parallel *_parallel, Setup *setup)  :  parallel(_parallel)
{
  // Set Initial values
  inputFileName        = setup->get("DataOutput.InputFileName" , "");
  outputFileName       = setup->get("DataOutput.OutputFileName", "default.h5");
  info                 = setup->get("DataOutput.Info"          , "No information provided");
  bool allowOverwrite  = setup->get("DataOutput.Overwrite", 0) || (setup->flags & Setup::GKC_OVERWRITE);
  
  dataFileFlushTiming  = Timing(setup->get("DataOutput.Flush.Step", -1), setup->get("DataOutput.Flush.Time", 100.)); 
  resumeFile           = inputFileName != "";
  
#pragma warning (disable : 1875) // ignore warnings about non-POD types
  { // Create compound data types 
    timing_tid = H5Tcreate(H5T_COMPOUND, sizeof(Timing));
    H5Tinsert(timing_tid, "Timestep", HOFFSET(Timing, step), H5T_NATIVE_INT   );
    H5Tinsert(timing_tid, "Time"    , HOFFSET(Timing, time), H5T_NATIVE_DOUBLE);

    // do not changes r and i name otherwise it will break compatibility with pyTables
    // Note that this should be binary compatible with C/C++ complex numbers
    typedef struct ComplexSplit_t {
      double r;   ///< real part
      double i;   ///< imaginary part
    } ComplexSplit;

    complex_tid = H5Tcreate(H5T_COMPOUND, sizeof (ComplexSplit_t));
    H5Tinsert(complex_tid, "r", HOFFSET(ComplexSplit_t, r), H5T_NATIVE_DOUBLE);
    H5Tinsert(complex_tid, "i", HOFFSET(ComplexSplit_t, i), H5T_NATIVE_DOUBLE);
    
    vector3D_tid = H5Tcreate(H5T_COMPOUND, sizeof (Vector3D));
    H5Tinsert(vector3D_tid, "x", HOFFSET(Vector3D, x), H5T_NATIVE_DOUBLE);
    H5Tinsert(vector3D_tid, "y", HOFFSET(Vector3D, y), H5T_NATIVE_DOUBLE);
    H5Tinsert(vector3D_tid, "z", HOFFSET(Vector3D, z), H5T_NATIVE_DOUBLE);
  }
#pragma warning (enable  : 1875)

  hsize_t species_dim[1]   = { setup->get("Grid.Ns", 1) }; 
  species_tid = H5Tarray_create(H5T_NATIVE_DOUBLE, 1, species_dim);
  
  hsize_t specfield_dim[2] = { setup->get("Grid.Ns", 1), setup->get("Plasma.Beta", 0.) == 0. ? 1 : 2 }; 
  specfield_tid = H5Tarray_create(H5T_NATIVE_DOUBLE, 2, specfield_dim);
    
  // BUG : Somehow HDF-8 stores only up to 8 chars of 64 possible. The rest is truncated ! Why ?
  // H5Tset_size(str_tid, H5T_VARIABLE) ..  not supported by HDF-5 parallel yet ....
  str_tid = H5Tcopy(H5T_C_S1); H5Tset_size(str_tid, 64); H5Tset_strpad(str_tid, H5T_STR_NULLTERM);

  // Create/Load HDF5 file
  if(resumeFile == false || (inputFileName != outputFileName)) create(setup, allowOverwrite);
}

FileIO::~FileIO()  
{
  check(H5LTset_attribute_string(file, ".", "StopTime", System::getTimeString().c_str()), DMESG("HDF-5 Error"));
  // Free all HDF5 resources

  // close some extra stuff
  check( H5Tclose(complex_tid), DMESG("HDF-5 Error"));
  check( H5Tclose(timing_tid ), DMESG("HDF-5 Error"));
  check( H5Tclose(species_tid), DMESG("HDF-5 Error"));
  check( H5Tclose(str_tid    ), DMESG("HDF-5 Error"));

  // close file
  check( H5Fclose(file)    , DMESG("HDF-5 Error : Unable to close file ..."));
}

void FileIO::create(Setup *setup, bool allowOverwrite) 
{
  hid_t file_apl = H5Pcreate(H5P_FILE_ACCESS);

#ifdef GKC_PARALLEL_MPI
    MPI_Info file_info;
    check(MPI_Info_create(&file_info), DMESG("HDF-5 Error"));
    // Note that using MPI_Info_set we can optimize HDF-5 MPI/IO writes

    check( H5Pset_fapl_mpio(file_apl, parallel->Comm[DIR_ALL], file_info), DMESG("HDF-5 Error"));

    // shouldn't be freed before H5Pclose(file_apl) ?
    MPI_Info_free(&file_info);
#endif
  
  // Close file even if some objects are still open (should generate warning)
  H5Pset_fclose_degree(file_apl, H5F_CLOSE_STRONG);
 
  // Create new output file
  file = check(H5Fcreate(outputFileName.c_str(), (allowOverwrite ? H5F_ACC_TRUNC : H5F_ACC_EXCL),
          H5P_DEFAULT, file_apl ), DMESG("HDF-5 Error : File already exists ? use -f to overwrite..."));
     
  check( H5Pclose(file_apl), DMESG("HDF-5 Error"));

  //////////////////////////////////////////////////////////////// Info Group ////////////////////////////////////////////////////////
   
  hid_t infoGroup = newGroup("/Info");
   
  check(H5LTset_attribute_string(infoGroup, ".", "Output" , outputFileName.c_str()), DMESG("HDF-5 Error"));
  check(H5LTset_attribute_string(infoGroup, ".", "Input"  , inputFileName.c_str()) , DMESG("HDF-5 Error"));
  check(H5LTset_attribute_string(infoGroup, ".", "Version", PACKAGE_VERSION)       , DMESG("HDF-5 Error"));
  check(H5LTset_attribute_string(infoGroup, ".", "Info"   , info.c_str())          , DMESG("HDF-5 Error"));

  check(H5LTset_attribute_string(infoGroup, ".", "Config", setup->configFileString.c_str()), DMESG("HDF-5 Error"));

  check(H5LTset_attribute_string(file     , ".", "StartTime", System::getTimeString().c_str()), DMESG("HDF-5 Error"));

  // get & save HDF-5 version
  { 
    std::stringstream version;
    unsigned int majnum, minnum, relnum;
    H5get_libversion(&majnum, &minnum, &relnum);
    version << majnum << "." << minnum << "." << relnum << std::endl;
  
    check(H5LTset_attribute_string(infoGroup, ".", "HDF5version", version.str().c_str()), DMESG("HDF-5 Error"));
  }

  H5Gclose(infoGroup);
         
  ////////////// Wrote setup constants, ugly here /////////////////
  hid_t constantsGroup = newGroup("/Constants");

  // Should be in Setup (Store Setup Constants
  if (!setup->parser_constants.empty()) { 
   
    std::vector<std::string> const_vec = Setup::split(setup->parser_constants, ",");

    // for(auto key_value : Setup::split(setup->parser_constants, ","))
    for(int s = 0; s < const_vec.size(); s++) { 
     
       std::vector<std::string> key_value = Setup::split(const_vec[s],"=");
       double value = std::stod(key_value[1]);
       check(H5LTset_attribute_double(constantsGroup, ".", key_value[0].c_str(), &value, 1), DMESG("HDF-5 Error"));
    }
  }
  H5Gclose(constantsGroup);
}

void FileIO::initData(Setup *setup)
{

}

hid_t FileIO::newGroup(std::string name, hid_t parentNode)
{
  if (parentNode == -2) parentNode = getFileID();
  hid_t newGroup = check(H5Gcreate(parentNode, name.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), DMESG("HDF-5 Error"));
  return newGroup;
}

// from http://mail.hdfgroup.org/pipermail/hdf-forum_hdfgroup.org/2010-June/012076.html
// to prevent corruption of HDF-5 file (requires regular calls to this->flush() ] 
void FileIO::flush(Timing timing, double dt, bool force_flush)
{
  if(timing.check(dataFileFlushTiming, dt) || force_flush) H5Fflush(file, H5F_SCOPE_GLOBAL);
}

void FileIO::printOn(std::ostream &output) const 
{
  output << "            -------------------------------------------------------------------" << std::endl
         << "Data       |  Input : " << (inputFileName == "" ? "---None---" : inputFileName)  << std::endl 
         << "           | Output : " <<  outputFileName  << " Resume : " << (resumeFile ? "yes" : "no") << std::endl;
}

FileAttr* FileIO::newTiming(hid_t group, hsize_t offset, bool write)
{
   hsize_t timing_cdim  [1] = {1             };
   hsize_t timing_maxdim[1] = {H5S_UNLIMITED };
   hsize_t timing_dim   [1] = {1             };
   
   FileAttr *Attr = new FileAttr("Time", group, file, 1, timing_dim, timing_maxdim, timing_cdim, &offset,  
                                  timing_cdim, &offset, parallel->myRank == 0, timing_tid);
   return Attr;
}

