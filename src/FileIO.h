/*
 * =====================================================================================
 *
 *       Filename: FileIO.h
 *
 *    Description: Data Input/Output using HDF-5 initialization routines
 *
 *         Author: Paul P. Hilscher (2010-), 
 *
 *        License: GPLv3+
 * =====================================================================================
 */

#ifndef __FILEIO_H_
#define __FILEIO_H_

#include "Global.h"

#include "Parallel/Parallel.h"
#include "Setup.h"
#include "Timing.h"

#include "SHDF5/FileAttr.h"
#include "SHDF5/TableAttr.h"



class Visualization;

/**
*   @brief class for reading/writing data using HDF5
*
*  This class performs readind(resume previous simulation) and
*  writing Data. Additionally a Table is provided in which
*  Take care, we use Fortran Array syntax
*  This example writes data to the HDF5 file.
*  Data conversion is performed during write operation.  
*  
**/
class FileIO : public IfaceGKC 
{
  Parallel *parallel;

  Timing dataFileFlushTiming; ///< Timing when to flush the HDF-5 file
   
  // Data files
  string outputFileName;    ///< Name of the output file
  string info;              ///< additional information to append to the file

 public:
  
  /**
  *   @brief constructor
  *
  *   Accepts following Setup parameters
  *
  *
  **/
  FileIO(Parallel *parallel, Setup *setup);
  
  virtual ~FileIO();
   
  string inputFileName;     ///< Input file name (move to protected!)

  /** 
  *
  * @brief Flush all data to disk to prevent corruption 
  *
  * Not working anyway. If program crashes data is basically
  * lost as the HDF-5 file becomes corrupted if not closed properly.
  * Snapshots cannot be saved.
  *
  * Well , http://www.hdfgroup.org/HDF5/doc/Advanced/HDF5_Metadata/index.html
  * say's different, but I don't get them ...
  *
  **/
  void flush(Timing timing, double dt);
   
  hid_t species_tid; ///< species HDF-5 type id (where is it used ?)
   
  hid_t s256_tid;   ///< string datatype
   
  hid_t file; ///< main data file id 
   
  hid_t timing_tid  ,  ///< Type id for Timing 
        complex_tid ,  ///< Complex Data type
        vector3D_tid;  ///< Vector (x,y,z) type

  hid_t getFileID() const { return file; };

  // move to private
  bool resumeFile, overwriteFile;

  hid_t  newGroup(std::string name, hid_t parentNode=-2);
        
  FileAttr *newTiming(hid_t group, hsize_t offset=0, bool write=1);

 protected:

  virtual void printOn(std::ostream &output) const;
  virtual void initData(Setup *setup);
  virtual void writeData(const Timing &timing, const double dt) {};
  virtual void closeData() {};

 private:

  void create(Setup *setup);

};

#endif // __FILEIO_H_
