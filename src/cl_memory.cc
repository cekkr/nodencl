/* Copyright 2018 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "cl_memory.h"
#include "noden_context.h"
#include "noden_program.h"
#include "noden_util.h"
#include <cstring>

class iGpuAccess {
public:
  virtual ~iGpuAccess() {}
  virtual cl_int unmapMem(uint32_t queueNum) = 0;
  virtual cl_int getKernelMem(iRunParams *runParams, bool isImageParam,
                              iKernelArg::eAccess access, bool &isSVM, void *&kernelMem, uint32_t queueNum) = 0;
  virtual void onGpuReturn() = 0;
};

class gpuMemory : public iGpuMemory {
public:
  gpuMemory(iGpuAccess *gpuAccess)
    : mGpuAccess(gpuAccess) {}
  ~gpuMemory() {
    mGpuAccess->onGpuReturn();
  }

  cl_int setKernelParam(cl_kernel kernel, uint32_t paramIndex, bool isImageParam,
                        iKernelArg::eAccess access, iRunParams *runParams, uint32_t queueNum) {
    cl_int error = CL_SUCCESS;
    error = mGpuAccess->unmapMem(queueNum);
    PASS_CL_ERROR;

    bool isSVM = false;
    void *kernelMem = nullptr;
    error = mGpuAccess->getKernelMem(runParams, isImageParam, access, isSVM, kernelMem, queueNum);
    PASS_CL_ERROR;

    error = clSetKernelArg(kernel, paramIndex, sizeof(cl_mem), kernelMem);
    return error;
  }

private:
  iGpuAccess *mGpuAccess;
};

class clMemory : public iClMemory, public iGpuAccess {
public:
  clMemory(cl_context context, std::vector<cl_command_queue> commandQueues, eMemFlags memFlags, eSvmType svmType, 
           uint32_t numBytes, deviceInfo *devInfo, const std::array<uint32_t, 3>& imageDims)
    : mContext(context), mCommandQueues(commandQueues), mMemFlags(memFlags), mSvmType(svmType),
      mNumBytes(numBytes), mDevInfo(devInfo), mImageDims(imageDims),
      mPinnedMem(nullptr), mImageMem(nullptr), mHostBuf(nullptr), mGpuLocked(false), mHostMapped(false),
      mMapFlags(eMemFlags::NONE), mMemLatest(eMemLatest::BUFFER) {}
  ~clMemory() {
    freeAllocation();
  }

  bool allocate() {
    cl_int error = CL_SUCCESS;
    cl_mem_flags clMemFlags = (eMemFlags::READONLY == mMemFlags) ? CL_MEM_READ_ONLY :
                              (eMemFlags::WRITEONLY == mMemFlags) ? CL_MEM_WRITE_ONLY :
                              CL_MEM_READ_WRITE;
    cl_mem_flags clSvmMemFlags = clMemFlags;
    //if (eSvmType::FINE == mSvmType) clSvmMemFlags |= CL_MEM_SVM_FINE_GRAIN_BUFFER;

    switch (mSvmType) {
    case eSvmType::FINE:
    case eSvmType::COARSE:
      //mHostBuf = clSVMAlloc(mContext, clSvmMemFlags, mNumBytes, 0);
      //mPinnedMem = clCreateBuffer(mContext, clMemFlags | CL_MEM_USE_HOST_PTR, mNumBytes, mHostBuf, &error);
      printf("SVM removed 424893");
      break;
    case eSvmType::NONE:
    default:
      mPinnedMem = clCreateBuffer(mContext, clMemFlags | CL_MEM_ALLOC_HOST_PTR, mNumBytes, nullptr, &error);
      if (CL_SUCCESS == error) {
        cl_map_flags clMapFlags = (eMemFlags::READONLY == mMemFlags) ? CL_MAP_WRITE_INVALIDATE_REGION : 
                                  (eMemFlags::WRITEONLY == mMemFlags) ? CL_MAP_READ :
                                  CL_MAP_READ | CL_MAP_WRITE;
        mHostBuf = clEnqueueMapBuffer(mCommandQueues[0], mPinnedMem, CL_TRUE, clMapFlags, 0, mNumBytes, 0, nullptr, nullptr, nullptr);
      } else
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
      mHostMapped = true;
      mMapFlags = (eMemFlags::READONLY == mMemFlags) ? eMemFlags::WRITEONLY : eMemFlags::READWRITE;
      break;
    }

    return nullptr != mHostBuf;
  }

  std::shared_ptr<iGpuMemory> getGPUMemory() {
    // printf("getGpuMemory type %d, host mapped %s, numBytes %d\n", mSvmType, mHostMapped?"true":"false", mNumBytes);
    mGpuLocked = true;
    return std::make_shared<gpuMemory>(this);
  }

  cl_int setHostAccess(eMemFlags haFlags, uint32_t queueNum) {
    cl_int error = CL_SUCCESS;
    if (mGpuLocked) {
      printf("GPU buffer access must be released before host access - %d\n", mNumBytes);
      error = CL_MAP_FAILURE;
      return error;
    }

    if (mHostMapped && (haFlags != mMapFlags)) {
      error = unmapMem(queueNum); // must unmap if host access flags don't match
      PASS_CL_ERROR;
    }

    if (!mHostMapped && !(eMemFlags::NONE == haFlags)) {
      cl_map_flags mapFlags = (eMemFlags::READWRITE == haFlags) ? CL_MAP_WRITE | CL_MAP_READ :
                              (eMemFlags::WRITEONLY == haFlags) ? CL_MAP_WRITE_INVALIDATE_REGION :
                              CL_MAP_READ;
      if (mImageMem) { // && (mDevInfo->oclVer < clVersion(2,0))) {
        if (eMemFlags::WRITEONLY == haFlags)
          mMemLatest = eMemLatest::BUFFER;
        else {
          error = copyImageToBuffer(queueNum);
          PASS_CL_ERROR;
        }
      }

      cl_bool blockingMap = mCommandQueues.size() > 1 ? CL_NON_BLOCKING : CL_BLOCKING;
      if (eSvmType::NONE == mSvmType) {
        void *hostBuf = clEnqueueMapBuffer(getCommandQueue(queueNum), mPinnedMem, blockingMap, mapFlags, 0, mNumBytes, 0, nullptr, nullptr, &error);
        PASS_CL_ERROR;
        if (mHostBuf != hostBuf) {
          printf("Unexpected behaviour - mapped buffer address is not the same: %p != %p\n", mHostBuf, hostBuf);
          error = CL_MAP_FAILURE;
          return error;
        }
        mHostMapped = true;
      } else if (eSvmType::COARSE == mSvmType) {
        /*error = clEnqueueSVMMap(getCommandQueue(queueNum), blockingMap, mapFlags, mHostBuf, mNumBytes, 0, nullptr, nullptr);
        PASS_CL_ERROR;
        mHostMapped = true;*/
        printf("SVM removed 2954354839");
      }

      mMapFlags = haFlags;
    }
    return error;
  }

  cl_int copyFrom(const void *srcBuf, size_t numBytes, uint32_t queueNum) {
    cl_int error = CL_SUCCESS;

    // if (eSvmType::NONE == mSvmType)
      memcpy(mHostBuf, srcBuf, numBytes);
    // else
    //   error = clEnqueueSVMMemcpy(getCommandQueue(queueNum), CL_BLOCKING, mHostBuf, srcBuf, numBytes, 0, nullptr, nullptr);
    // PASS_CL_ERROR;
    return error;
  }

  void freeAllocation() {
    cl_int error = CL_SUCCESS;
    error = unmapMem(0);
    if (CL_SUCCESS != error)
      printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
        __FILE__, __LINE__, error, clGetErrorString(error));

    // SVM removed for OpenCL 1.2
    //if (mHostBuf && (eSvmType::NONE != mSvmType)) clSVMFree(mContext, mHostBuf);

    if (mImageMem) {
      error = clReleaseMemObject(mImageMem);
      if (CL_SUCCESS != error)
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
    }

    if (mPinnedMem) {
      error = clReleaseMemObject(mPinnedMem);
      if (CL_SUCCESS != error)
        printf("OpenCL error in subroutine. Location %s(%d). Error %i: %s\n",
          __FILE__, __LINE__, error, clGetErrorString(error));
    }

    mPinnedMem = nullptr;
    mImageMem = nullptr;
    mHostBuf = nullptr;
  }

  uint32_t numBytes() const { return mNumBytes; }
  eMemFlags memFlags() const { return mMemFlags; }
  eSvmType svmType() const { return mSvmType; }
  std::string svmTypeName() const {
    switch (mSvmType) {
    case eSvmType::FINE: return "fine";
    case eSvmType::COARSE: return "coarse";
    case eSvmType::NONE: return "none";
    default: return "unknown";
    }
  }
  void* hostBuf() const { return mHostBuf; }
  bool hasDimensions() const { return mImageDims[0] > 0; }

  enum class eMemLatest : uint8_t { BUFFER = 0, SAME = 1, IMAGE = 2 };

private:
  cl_context mContext;
  std::vector<cl_command_queue> mCommandQueues;
  const eMemFlags mMemFlags;
  const eSvmType mSvmType;
  const uint32_t mNumBytes;
  deviceInfo *mDevInfo;
  const std::array<uint32_t, 3> mImageDims;
  cl_mem mPinnedMem;
  cl_mem mImageMem;
  void *mHostBuf;
  bool mGpuLocked;
  bool mHostMapped;
  eMemFlags mMapFlags;
  eMemLatest mMemLatest;

  cl_command_queue getCommandQueue(uint32_t queueNum) {
    uint32_t q = queueNum;
    if (queueNum >= (uint32_t)mCommandQueues.size()) {
      printf("Invalid queue \'%d\', defaulting to 0\n", queueNum);
      q = 0;
    }
    return mCommandQueues.at(q);
  }

  cl_int unmapMem(uint32_t queueNum) {
    cl_int error = CL_SUCCESS;
    if (mHostMapped) {
      if (eSvmType::NONE == mSvmType)
        error = clEnqueueUnmapMemObject(getCommandQueue(queueNum), mPinnedMem, mHostBuf, 0, nullptr, nullptr);
      else if (eSvmType::COARSE == mSvmType){
        printf("SVM removed 234892345");
        //error = clEnqueueSVMUnmap(getCommandQueue(queueNum), mHostBuf, 0, 0, nullptr);
      }
      mHostMapped = false;
      mMapFlags = eMemFlags::NONE;
    }
    return error;
  }

  cl_int copyImageToBuffer(uint32_t queueNum) {
    cl_int error = CL_SUCCESS;
    if (mImageMem) {
      const size_t origin[3] = { 0, 0, 0 };
      size_t region[3] = { 1, 1, 1 };
      error = clGetImageInfo(mImageMem, CL_IMAGE_WIDTH, sizeof(size_t), &region[0], NULL);
      PASS_CL_ERROR;
      size_t height = 0;
      error = clGetImageInfo(mImageMem, CL_IMAGE_HEIGHT, sizeof(size_t), &height, NULL);
      PASS_CL_ERROR;
      if (height) region[1] = height;
      size_t depth = 0;
      error = clGetImageInfo(mImageMem, CL_IMAGE_DEPTH, sizeof(size_t), &depth, NULL);
      PASS_CL_ERROR;
      if (depth) region[2] = depth;

      // printf("Copying image memory to buffer size %zdx%zd\n", region[0], region[1]);
      error = clEnqueueCopyImageToBuffer(getCommandQueue(queueNum), mImageMem, mPinnedMem, origin, region, 0, 0, nullptr, nullptr);
      PASS_CL_ERROR;
      mMemLatest = eMemLatest::SAME;
    }
    return error;
  }

  cl_int getKernelMem(iRunParams *runParams, bool isImageParam,
                      iKernelArg::eAccess access, bool &isSVM, void *&kernelMem, uint32_t queueNum) {
    kernelMem = mImageMem ? &mImageMem : &mPinnedMem;
    const size_t origin[3] = { 0, 0, 0 };
    cl_int error = CL_SUCCESS;

    if (isImageParam) {
      if (!mImageMem) {
        // create new image object
        cl_image_format clImageFormat;
        memset(&clImageFormat, 0, sizeof(clImageFormat));
        clImageFormat.image_channel_order = CL_RGBA;
        clImageFormat.image_channel_data_type = CL_FLOAT;

        cl_image_desc clImageDesc;
        memset(&clImageDesc, 0, sizeof(clImageDesc));
        clImageDesc.image_type = runParams->numDims() > 2 ? CL_MEM_OBJECT_IMAGE3D : CL_MEM_OBJECT_IMAGE2D;
        clImageDesc.image_width = mImageDims[0];
        clImageDesc.image_height = runParams->numDims() > 1 ? mImageDims[1] : 1;
        clImageDesc.image_depth = runParams->numDims() > 2 ? mImageDims[2] : 1;
        // if (mDevInfo->oclVer >= clVersion(2,0))
        //   clImageDesc.mem_object = mPinnedMem;

        cl_mem_flags clMemFlags = (eMemFlags::READONLY == mMemFlags) ? CL_MEM_READ_ONLY :
                                  (eMemFlags::WRITEONLY == mMemFlags) ? CL_MEM_WRITE_ONLY :
                                  CL_MEM_READ_WRITE;
        mImageMem = clCreateImage(mContext, clMemFlags | CL_MEM_HOST_NO_ACCESS, &clImageFormat, &clImageDesc, nullptr, &error);
        PASS_CL_ERROR;

        kernelMem = &mImageMem;
      }

      // if (mDevInfo->oclVer < clVersion(2,0)) {
        if (iKernelArg::eAccess::WRITEONLY == access)
          mMemLatest = eMemLatest::IMAGE;
        else if (eMemLatest::BUFFER == mMemLatest) {
          // printf("Copying image memory from buffer size %dx%d\n", mImageDims[0], mImageDims[1]);
          size_t region[3] = { 1, 1, 1 };
          for (size_t i = 0; i < runParams->numDims(); ++i)
            region[i] = mImageDims[i];
          error = clEnqueueCopyBufferToImage(getCommandQueue(queueNum), mPinnedMem, mImageMem, 0, origin, region, 0, nullptr, nullptr);
          PASS_CL_ERROR;
        }
      // }
    } else if (mImageMem) {
      // copy back from image if required, leave image allocation allocated
      if (/*(mDevInfo->oclVer < clVersion(2,0)) &&*/ (eMemLatest::IMAGE == mMemLatest)) {
        error = copyImageToBuffer(queueNum);
        PASS_CL_ERROR;
      }
      kernelMem = &mPinnedMem;
    }

    isSVM = (eSvmType::NONE != mSvmType) && !isImageParam;
    if (isSVM)
      kernelMem = mHostBuf;

    return error;
  }

  void onGpuReturn() {
    mGpuLocked = false;
  }
};

iClMemory *iClMemory::create(cl_context context, std::vector<cl_command_queue> commandQueues, eMemFlags memFlags, eSvmType svmType,
                             uint32_t numBytes, deviceInfo *devInfo, const std::array<uint32_t, 3>& imageDims) {
  return new clMemory(context, commandQueues, memFlags, svmType, numBytes, devInfo, imageDims);
}
