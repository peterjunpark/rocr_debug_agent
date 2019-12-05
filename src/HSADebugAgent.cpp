////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
//
// Copyright (c) 2018, Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////


#include <atomic>
#include <sys/types.h>
#include <dirent.h>

// HSA headers
#include <hsakmt.h>
#include <hsa_api_trace.h>

// Debug Agent Headers
#include "AgentLogging.h"
#include "AgentUtils.h"
#include "HSADebugAgent.h"
#include "HSADebugInfo.h"
#include "HSAIntercept.h"
#include "HSAHandleLinuxSignals.h"
#include "HSAHandleMemoryFault.h"

// Debug info tracked by debug agent, it is probed by ROCm-GDB
RocmGpuDebug _r_rocm_debug_info =
{
    HSA_DEBUG_AGENT_VERSION, nullptr, nullptr
};

// Temp direcoty path for code object files
char g_codeObjDir[92];

// whether delete tmp code object files
bool g_deleteTmpFile = true;

// If debug agent is successfully loaded and initialized
std::atomic<bool> g_debugAgentInitialSuccess{false};

// Debug trap handler code object reader
hsa_code_object_reader_t debugTrapHandlerCodeObjectReader = {0};

// Debug trap handler executable
hsa_executable_t debugTrapHandlerExecutable = {0};

// lock for access debug agenet
std::mutex debugAgentAccessLock;

// Initial debug agent info link list
static DebugAgentStatus AgentInitDebugInfo();

// Get agent info in the callback when query agents
static hsa_status_t QueryAgentCallback(hsa_agent_t agent, void *pData);

// Get ISA info in the callback when query ISAs
static hsa_status_t QueryAgentISACallback(hsa_isa_t isa, void *pData);

// Check runtime version
static DebugAgentStatus AgentCheckVersion(uint64_t runtimeVersion,
                                          uint64_t failedToolCount,
                                          const char *const *pFailedToolNames);

// Clean _r_rocm_debug_info
static void AgentCleanDebugInfo();

// Set system event handler in runtime
static DebugAgentStatus AgentSetSysEventHandler();

// Handle runtime event based on event type.
static hsa_status_t
HSADebugAgentHandleRuntimeEvent(const hsa_amd_event_t* event, void* pData);

extern "C" bool OnLoad(void *pTable,
                       uint64_t runtimeVersion, uint64_t failedToolCount,
                       const char *const *pFailedToolNames)
{
    g_debugAgentInitialSuccess = false;
    DebugAgentStatus status = DEBUG_AGENT_STATUS_FAILURE;
    uint32_t tableVersionMajor =
            (reinterpret_cast<HsaApiTable *>(pTable))->version.major_id;
    uint32_t tableVersionMinor =
            (reinterpret_cast<HsaApiTable *>(pTable))->version.minor_id;

    status = AgentInitLogger();

    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Cannot initialize logging");
        return false;
    }

    AGENT_LOG("===== Load GDB Tools Agent=====");

    status = AgentCheckVersion(runtimeVersion, failedToolCount, pFailedToolNames);
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Version mismatch");
        return false;
    }

    // Check function table version.
    if (tableVersionMajor < 1 || tableVersionMinor < 48)
    {
        AGENT_ERROR("Unsupported runtime version");
        return false;
    }

    status = AgentInitDebugInfo();
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Cannot initialize debug info");
        return false;
    }

    status = AgentCreateTmpDir();
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Cannot create code object directory");
        return false;
    }

    // Set the custom runtime event handler
    status = AgentSetSysEventHandler();
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Interception: Cannot set GPU event handler");
        return false;
    }

    status = InitHsaCoreAgentIntercept(
            reinterpret_cast<HsaApiTable *>(pTable));
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        AGENT_ERROR("Cannot initialize dispatch tables");
        return false;
    }

    InitialLinuxSignalsHandler();

    AGENT_LOG("===== Finished Loading ROC Debug Agent=====");
    g_debugAgentInitialSuccess.store(true, std::memory_order_release);

    return true;
}

extern "C" void OnUnload()
{
    std::lock_guard<std::mutex> lock(debugAgentAccessLock);

    AGENT_LOG("===== Unload ROC Debug Agent=====");

    DebugAgentStatus retVal = DEBUG_AGENT_STATUS_SUCCESS;
    DebugAgentStatus status = DEBUG_AGENT_STATUS_FAILURE;

    AgentCleanDebugInfo();

    status = AgentCloseLogger();
    if (status != DEBUG_AGENT_STATUS_SUCCESS)
    {
        if (retVal == DEBUG_AGENT_STATUS_SUCCESS)
        {
            retVal = status;
        }
        AGENT_ERROR("OnUnload: Cannot close Logging");
    }
}

// Check the version based on the provided by HSA runtime's OnLoad function.
// The logic is based on code in the HSA profiler (HSAPMCAgent.cpp).
// Return success if the versions match.
static DebugAgentStatus AgentCheckVersion(uint64_t runtimeVersion,
                                          uint64_t failedToolCount,
                                          const char *const *pFailedToolNames)
{
    DebugAgentStatus status = DEBUG_AGENT_STATUS_FAILURE;
    static const std::string ROCM_DEBUG_AGENT_LIB("libAMDHSADebugAgent-x64.so");

    if (failedToolCount > 0 && runtimeVersion > 0)
    {
        if (pFailedToolNames != nullptr)
        {
            for (uint64_t i = 0; i < failedToolCount; i++)
            {
                if (pFailedToolNames[i] != nullptr)
                {
                    std::string failedToolName = std::string(pFailedToolNames[i]);

                    if (std::string::npos != failedToolName.find_last_of(ROCM_DEBUG_AGENT_LIB))
                    {
                        AGENT_OP("rocm-gdb not enabled. Version mismatch between ROCm runtime and "
                                 << ROCM_DEBUG_AGENT_LIB);
                        AGENT_ERROR("Debug agent not enabled. Version mismatch between ROCm runtime and "
                                    << ROCM_DEBUG_AGENT_LIB);
                    }
                }
                else
                {
                    AGENT_ERROR("Debug agent not enabled," << ROCM_DEBUG_AGENT_LIB
                                                           << "version could not be verified");
                    AGENT_ERROR("AgentCheckVersion: pFailedToolNames[" << i << "] is nullptr");
                }
            }
            return status;
        }
        else
        {
            AGENT_ERROR("AgentCheckVersion: Cannot verify version successfully");
        }
    }
    else
    {
        status = DEBUG_AGENT_STATUS_SUCCESS;
    }

    return status;
}

static DebugAgentStatus AgentInitDebugInfo()
{
    AGENT_LOG("Initialize agent debug info")

    hsa_status_t status = HSA_STATUS_SUCCESS;

    GPUAgentInfo *pEndGPUAgentInfo = nullptr;
    status = hsa_iterate_agents(QueryAgentCallback, &(pEndGPUAgentInfo));
    if (status != HSA_STATUS_SUCCESS)
    {
        AGENT_ERROR("Failed querying the device information.");
        return DEBUG_AGENT_STATUS_FAILURE;
    }

    AGENT_LOG("Finished initializing agent debug info")

    return DEBUG_AGENT_STATUS_SUCCESS;
}

static hsa_status_t GetGpuId(hsa_agent_t agent, uint32_t *gpuId)
{
    static const std::string sysfs_nodes_path (
        "/sys/devices/virtual/kfd/kfd/topology/nodes/");

    uint32_t location_id;
    if (hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_BDFID),
            &location_id) != HSA_STATUS_SUCCESS)
        return HSA_STATUS_ERROR;

    auto *dirp = opendir (sysfs_nodes_path.c_str ());
    if (!dirp)
        return HSA_STATUS_ERROR;

    struct dirent *dir;
    while ((dir = readdir (dirp)) != 0)
    {
        if (!strcmp (dir->d_name, ".") || !strcmp (dir->d_name, ".."))
            continue;

        std::string node_path (sysfs_nodes_path + dir->d_name);
        std::ifstream props_ifs (node_path + "/properties");
        if (!props_ifs.is_open ())
            continue;

        std::string prop_name;
        uint64_t prop_value;
        while (props_ifs >> prop_name >> prop_value)
        {
            if (!prop_name.compare ("location_id"))
            {
                if (location_id != static_cast<uint32_t> (prop_value))
                    break;

                /* Retrieve the GPU ID.  */
                std::ifstream gpu_id_ifs (node_path + "/gpu_id");
                if (!gpu_id_ifs.is_open ())
                    return HSA_STATUS_ERROR;

                gpu_id_ifs >> *gpuId;
                return HSA_STATUS_SUCCESS;
            }
        }
    }
    return HSA_STATUS_ERROR;
}

static hsa_status_t QueryAgentCallback(hsa_agent_t agent, void *pData)
{
    // Add the GPU agent to the end of the agent list
    if (pData == nullptr)
    {
        AGENT_ERROR("QueryAgentCallback: Invalid argument pData");
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }

    int32_t status = HSA_STATUS_SUCCESS;
    // Find out the device type and skip it if it's a CPU.
    hsa_device_type_t deviceType;
    status = hsa_agent_get_info(
            agent, HSA_AGENT_INFO_DEVICE, &deviceType);
    if (status == HSA_STATUS_SUCCESS && deviceType == HSA_DEVICE_TYPE_CPU)
    {
        return HSA_STATUS_SUCCESS;
    }

    GPUAgentInfo *pGpuAgent = new GPUAgentInfo;
    memset(pGpuAgent, 0, sizeof(GPUAgentInfo));

    pGpuAgent->agent = reinterpret_cast<void*>(agent.handle);
    pGpuAgent->pQueueList = nullptr;
    pGpuAgent->agentStatus = AGENT_STATUS_UNSUPPORTED;

    char nameBuf[2 * AGENT_MAX_AGENT_NAME_LEN];
    memset(nameBuf, 0, 2 * AGENT_MAX_AGENT_NAME_LEN);
    status |= hsa_agent_get_info(
            agent, HSA_AGENT_INFO_VENDOR_NAME, nameBuf);
    // Insert a space between the vendor and product names.
    size_t vendorNameLen = strnlen(nameBuf, AGENT_MAX_AGENT_NAME_LEN);
    nameBuf[vendorNameLen] = ' ';

    status |= hsa_agent_get_info(
            agent, HSA_AGENT_INFO_NAME, nameBuf + vendorNameLen + 1);
    memcpy(pGpuAgent->agentName, nameBuf, AGENT_MAX_AGENT_NAME_LEN);

    // TODO: HSA_AGENT_INFO_NODE is deprecated.
    // Check if HSA_AMD_AGENT_INFO_DRIVER_NODE_ID can be used instead.
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AGENT_INFO_NODE),
            &(pGpuAgent->nodeId));
    status |= GetGpuId(agent, &pGpuAgent->gpuId);
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_CHIP_ID),
            &(pGpuAgent->chipID));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT),
            &(pGpuAgent->numCUs));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES),
            &(pGpuAgent->numSEs));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU),
            &(pGpuAgent->numSIMDsPerCU));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU),
            &(pGpuAgent->wavesPerCU));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY),
            &(pGpuAgent->maxEngineFreq));
    status |= hsa_agent_get_info(
            agent,
            static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_MEMORY_MAX_FREQUENCY),
            &(pGpuAgent->maxMemoryFreq));
    if (status != HSA_STATUS_SUCCESS)
    {
        AGENT_WARNING("Failed to get some of the device info");
    }

    pGpuAgent->hasAccVgprs = false;
    status = hsa_agent_iterate_isas(
            agent, QueryAgentISACallback, pGpuAgent);
    if (status != HSA_STATUS_SUCCESS)
    {
        AGENT_ERROR("Cannot get supported ISA(s) for agent "
                    << pGpuAgent->agentName);
        return HSA_STATUS_ERROR;
    }

    if (pGpuAgent->agentStatus != AGENT_STATUS_ACTIVE)
    {
        AGENT_WARNING("Do not support agent " << pGpuAgent->agentName);
    }

    GPUAgentInfo *pPrevGPUAgent = *(GPUAgentInfo **)pData;
    pGpuAgent->pPrev = pPrevGPUAgent;
    if (pPrevGPUAgent == nullptr)
    {
        _r_rocm_debug_info.pAgentList = pGpuAgent;
    }
    else
    {
        pPrevGPUAgent->pNext = pGpuAgent;
    }
    // Update pData to point to the end of the link list,
    // and pass through agent iteration.
    *(GPUAgentInfo **)pData = pGpuAgent;

    return HSA_STATUS_SUCCESS;
}

static hsa_status_t QueryAgentISACallback(hsa_isa_t isa, void *pData)
{
    if (pData == nullptr)
    {
        return HSA_STATUS_ERROR;
    }

    char isaName[AGENT_MAX_AGENT_NAME_LEN];
    hsa_status_t status = hsa_isa_get_info_alt(
            isa, HSA_ISA_INFO_NAME, isaName);

    if ((strcmp(isaName, gfx900) == 0) ||
        (strcmp(isaName, gfx906) == 0))
    {
        ((GPUAgentInfo *)pData)->agentStatus = AGENT_STATUS_ACTIVE;
    }
    else if (strcmp(isaName, gfx908) == 0)
    {
        ((GPUAgentInfo *)pData)->agentStatus = AGENT_STATUS_ACTIVE;
        ((GPUAgentInfo *)pData)->hasAccVgprs = true;
    }

    return status;
}

static void AgentCleanDebugInfo()
{
    /* delete agent list*/
    GPUAgentInfo *pAgent = _r_rocm_debug_info.pAgentList;
    GPUAgentInfo *pAgentNext = nullptr;
    while (pAgent != nullptr)
    {
        pAgentNext = pAgent->pNext;
        QueueInfo *pQueue = pAgent->pQueueList;
        QueueInfo *pQueueListNext = nullptr;
        while (pQueue != nullptr)
        {
            pQueueListNext = pQueue->pNext;
            RemoveQueueFromList(pQueue->queueId);
            pQueue = pQueueListNext;
        }
        pAgent = pAgentNext;
    }

    /* delete executable list*/
    ExecutableInfo *pExec = _r_rocm_debug_info.pExecutableList;
    ExecutableInfo *pExecNext = nullptr;
    while (pExec != nullptr)
    {
        pExecNext = pExec->pNext;
        DeleteExecutableFromList(pExec->executableId);
        pExec = pExecNext;
    }

    /*delete temp files */
    if (g_deleteTmpFile)
    {
        AgentDeleteFile(g_codeObjDir);
    }
}

static DebugAgentStatus AgentSetSysEventHandler()
{
    hsa_status_t status = HSA_STATUS_SUCCESS;
    status = hsa_amd_register_system_event_handler(HSADebugAgentHandleRuntimeEvent, NULL);
    if (status == HSA_STATUS_SUCCESS)
    {
        return DEBUG_AGENT_STATUS_SUCCESS;
    }
    else
    {
        AGENT_ERROR("System event handler aleady exists");
        return DEBUG_AGENT_STATUS_FAILURE;
    }
}

static hsa_status_t
HSADebugAgentHandleRuntimeEvent(const hsa_amd_event_t* event, void* pData)
{
    if (event == nullptr)
    {
        AGENT_ERROR("HSA Runtime provided a nullptr event pointer.");
        return HSA_STATUS_ERROR;
    }
    hsa_amd_event_t gpuEvent = *event;
    switch (gpuEvent.event_type)
    {
        case HSA_AMD_GPU_MEMORY_FAULT_EVENT:
            return HSADebugAgentHandleMemoryFault(gpuEvent, pData);
        default :
            return HSA_STATUS_SUCCESS;
    }
}

