/*
 * Copyright (C) 2019-2020, Xilinx Inc - All rights reserved
 * Xilinx Resouce Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <boost/asio.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "xrm.h"
#include "xrm_system.hpp"

using boost::asio::ip::tcp;
namespace pt = boost::property_tree;

struct xrmPrivateContext {
    uint32_t xrmApiVersion;
    xrmLogLevelType xrmLogLevel;
    uint64_t xrmClientId;
    pthread_mutex_t lock;
    tcp::socket* socket;
    boost::asio::io_service* ioService;
    tcp::resolver* resolver;
};

enum { maxLength = 131072 };

static int32_t xrmJsonRequest(xrmContext context, const char* jsonReq, char* jsonRsp);
static void hexstrToBin(std::string& inStr, int32_t insz, unsigned char* out);
static void binToHexstr(unsigned char* in, int32_t insz, std::string& outStr);
static void xrmLog(xrmLogLevelType contextLogLevel, xrmLogLevelType logLevel, const char* format, ...);
static bool xrmIsCuExisting(xrmContext context, xrmCuProperty* cuProp);
static bool xrmIsCuListExisting(xrmContext context, xrmCuListProperty* cuListProp);
static bool xrmIsCuGroupExisting(xrmContext context, xrmCuGroupProperty* cuGroupProp);

/**
 * \brief Establishes a connection with the XRM daemon
 *
 * @param xrmApiVersion the XRM API version number
 * @return xrmContext, pointer to created context or NULL on fail
 */
xrmContext xrmCreateContext(uint32_t xrmApiVersion) {
    if (xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): wrong XRM API version: %d", __func__, xrmApiVersion);
        return (NULL);
    }

    xrmPrivateContext* ctx = new xrmPrivateContext;
    if (ctx == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): Fail to alloc context", __func__);
        return (NULL);
    }
    ctx->xrmApiVersion = XRM_API_VERSION_1;
    pthread_mutex_init(&ctx->lock, NULL);

    try {
        ctx->ioService = new boost::asio::io_service;
        ctx->socket = new tcp::socket(*ctx->ioService);
        ctx->resolver = new tcp::resolver(*ctx->ioService);

        boost::asio::connect(*ctx->socket, ctx->resolver->resolve({"127.0.0.1", "9763"}));
    } catch (std::exception& e) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s Exception: %s\n", __func__, e.what());
        if (ctx->socket) {
            /* disconnect first, then release resource */
            boost::system::error_code ec;
            ctx->socket->shutdown(tcp::socket::shutdown_both, ec);
            if (ec) {
                xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s: socket shutdown error %s = %d", __func__,
                       ec.category().name(), ec.value());
            }
            delete ctx->socket;
        }
        if (ctx->resolver) {
            ctx->resolver->cancel();
            delete ctx->resolver;
        }
        if (ctx->ioService) {
            ctx->ioService->stop();
            delete ctx->ioService;
        }
        ctx->socket = NULL;
        ctx->ioService = NULL;
        ctx->resolver = NULL;
        pthread_mutex_destroy(&ctx->lock);
        delete ctx;
        return (NULL);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree createContextTree;
    createContextTree.put("request.name", "createContext");
    createContextTree.put("request.requestId", 1);
    createContextTree.put("request.parameters.context", "readContext");
    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, createContextTree);
    if (xrmJsonRequest((xrmContext)ctx, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) {
        xrmDestroyContext(ctx);
        return (NULL);
    }
    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);
    auto logLevel = rspTree.get<int32_t>("response.status.value");
    ctx->xrmLogLevel = (xrmLogLevelType)logLevel;
    ctx->xrmClientId = rspTree.get<uint64_t>("response.data.clientId");

    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree echoContextTree;
    echoContextTree.put("request.name", "echoContext");
    echoContextTree.put("request.requestId", 1);
    echoContextTree.put("request.parameters.context", "echoContext");
    echoContextTree.put("request.parameters.echo", "echo");
    echoContextTree.put("request.parameters.clientId", ctx->xrmClientId);
    std::stringstream echoReqstr;
    boost::property_tree::write_json(echoReqstr, echoContextTree);
    if (xrmJsonRequest((xrmContext)ctx, echoReqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) {
        xrmDestroyContext(ctx);
        return (NULL);
    }
    return (ctx);
}

/**
 * \brief Disconnects an existing connection with the XRM daemon
 *
 * @param context the context created through xrmCreateContext()
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmDestroyContext(xrmContext context) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx != NULL) {
        if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
            xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
            return (XRM_ERROR);
        }

        char jsonRsp[maxLength];
        memset(jsonRsp, 0, maxLength * sizeof(char));
        pt::ptree destroyContextTree;
        destroyContextTree.put("request.name", "destroyContext");
        destroyContextTree.put("request.requestId", 1);
        destroyContextTree.put("request.parameters.context", "removeContext");
        destroyContextTree.put("request.parameters.echo", "echo");
        destroyContextTree.put("request.parameters.clientId", ctx->xrmClientId);
        std::stringstream reqstr;
        boost::property_tree::write_json(reqstr, destroyContextTree);
        if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);
        std::stringstream rspstr;
        rspstr << jsonRsp;
        pt::ptree rspTree;
        boost::property_tree::read_json(rspstr, rspTree);
        auto ret = rspTree.get<int32_t>("response.status.value");

        if (ctx->socket) {
            /* disconnect first, then release resource */
            boost::system::error_code ec;
            ctx->socket->shutdown(tcp::socket::shutdown_both, ec);
            if (ec) {
                xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s: socket shutdown error %s = %d", __func__,
                       ec.category().name(), ec.value());
                return (XRM_ERROR);
            }
            delete ctx->socket;
        }
        if (ctx->resolver) {
            ctx->resolver->cancel();
            delete ctx->resolver;
        }
        if (ctx->ioService) {
            ctx->ioService->stop();
            delete ctx->ioService;
        }
        ctx->socket = NULL;
        ctx->ioService = NULL;
        ctx->resolver = NULL;
        pthread_mutex_destroy(&ctx->lock);
        delete ctx;
        return (XRM_SUCCESS);
    } else {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s context pointer is NULL", __func__);
        return (XRM_ERROR_INVALID);
    }
}

/**
 * Internal function.
 *
 * \brief logs system message for XRM
 *
 * @param contextLogLevel the log level set through XRM context
 * @param logLevel requested log level
 * @param format, ...: message to be logged
 * @return void
 */
static void xrmLog(xrmLogLevelType contextLogLevel, xrmLogLevelType logLevel, const char* format, ...) {
    if (logLevel <= contextLogLevel) {
        va_list tmpArgs;
        va_start(tmpArgs, format);
        int32_t len = std::vsnprintf(NULL, 0, format, tmpArgs);
        va_end(tmpArgs);
        if (len < 0) return;

        ++len; //To include null terminator
        std::vector<char> msg(len);
        va_list args;
        va_start(args, format);
        len = std::vsnprintf(msg.data(), len, format, args);
        va_end(args);
        syslog(logLevel, "%s", msg.data());
    }
}

/**
 * Internal function.
 *
 * \brief sends a JSON request message to the XRM
 * daemon and copies a JSON response message to a caller
 * provided buffer.
 *
 * @param context the context created through xrmCreateContext()
 * @param jsonReq request JSON message
 * @param jsonRsp JSON response message
 * @return int32_t, 0 on success or appropriate error number
 **/
static int32_t xrmJsonRequest(xrmContext context, const char* jsonReq, char* jsonRsp) {
    int32_t rc = XRM_SUCCESS;
    boost::system::error_code ec;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || jsonReq == NULL || jsonRsp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s: context, jsonReq or jsonRsp pointer is NULL\n", __func__);
        rc = XRM_ERROR_INVALID;
        return (rc);
    }

    try {
        // Send request
        size_t reqLen = std::strlen(jsonReq);
        xrmLog(ctx->xrmLogLevel, XRM_LOG_NOTICE, "Sending %s\n", jsonReq);
        pthread_mutex_lock(&ctx->lock);
        boost::asio::write(*ctx->socket, boost::asio::buffer(jsonReq, reqLen), ec);

        // Get response
        xrmLog(ctx->xrmLogLevel, XRM_LOG_NOTICE, "Getting response");

        size_t replyLength = 0;
        while (ec != boost::asio::error::eof) {
            replyLength = ctx->socket->read_some(boost::asio::buffer(jsonRsp, maxLength), ec);
            /*
             * Be careful about this: need to make sure the jsonRsp[] is init to 0, otherwise
             * the jsonRsp[reply_length] is not 0 even there is NO more data coming in.
             */
            if (jsonRsp[replyLength] == 0) break;
        }
        pthread_mutex_unlock(&ctx->lock);

        xrmLog(ctx->xrmLogLevel, XRM_LOG_NOTICE, "%s\n", jsonRsp);

    } catch (std::exception& e) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s Exception: %s\n", __func__, e.what());
        rc = XRM_ERROR;
    }

    return (rc);
}

/**
 * \brief To check whether the daemon is running
 *
 * @param context the context created through xrmCreateContext()
 * @return bool, true on running or false on NOT running
 */
bool xrmIsDaemonRunning(xrmContext context) {
    bool ret = false;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree loadOneDeviceTree;
    loadOneDeviceTree.put("request.name", "isDaemonRunning");
    loadOneDeviceTree.put("request.requestId", 1);
    loadOneDeviceTree.put("request.parameters.echo", "echo");
    loadOneDeviceTree.put("request.parameters.echoClientId", "echo");
    loadOneDeviceTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, loadOneDeviceTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (ret);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        ret = true;
    }
    return (ret);
}

/**
 * \brief Loads xclbin to one device
 *
 * @param context the context created through xrmCreateContext()
 * @param deviceId the device id to load the xclbin file, -1 means to any available device
 * @param xclbinFileName xclbin file (full path and name)
 * @return int32_t, device id (>= 0) loaded with xclbin or appropriate error number (< 0) on fail
 */
int32_t xrmLoadOneDevice(xrmContext context, int32_t deviceId, char* xclbinFileName) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || xclbinFileName == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or xclbin file pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (xclbinFileName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s xclbin file name is not provided", __func__);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree loadOneDeviceTree;
    loadOneDeviceTree.put("request.name", "loadOneDevice");
    loadOneDeviceTree.put("request.requestId", 1);
    loadOneDeviceTree.put("request.parameters.deviceId", deviceId);
    loadOneDeviceTree.put("request.parameters.xclbinFileName", xclbinFileName);
    loadOneDeviceTree.put("request.parameters.echoClientId", "echo");
    loadOneDeviceTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, loadOneDeviceTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        auto loadedDevId = rspTree.get<int32_t>("response.data.deviceId");
        ret = loadedDevId;
    }
    return (ret);
}

/**
 * \brief Unloads xclbin from one device
 *
 * @param context the context created through xrmCreateContext()
 * @param deviceId the device id to unload the xclbin file
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmUnloadOneDevice(xrmContext context, int32_t deviceId) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree unloadOneDeviceTree;
    unloadOneDeviceTree.put("request.name", "unloadOneDevice");
    unloadOneDeviceTree.put("request.requestId", 1);
    unloadOneDeviceTree.put("request.parameters.deviceId", deviceId);
    unloadOneDeviceTree.put("request.parameters.echoClientId", "echo");
    unloadOneDeviceTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, unloadOneDeviceTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_NOTICE, "%s(): fail to unload from device %d", __func__, deviceId);
    } else {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_NOTICE, "%s(): success to unload xclbin", __func__);
    }
    return (ret);
}

/**
 * Internal function.
 *
 * \brief Convers hex string like "0123456789abcdef" to bin 0123456789abcdef
 * @param inStr input hex string
 * @param inze size of input
 * @param out output binary array
 * @return void
 **/
static void hexstrToBin(std::string& inStr, int32_t insz, unsigned char* out) {
    char in[insz];
    unsigned char* pout = out;
    unsigned char hex_0, hex_1;
    strncpy(in, inStr.c_str(), insz);
    for (int32_t i = 0; i < insz; i += 2, pout++) {
        if (in[i] < 'a')
            hex_0 = (in[i] - '0');
        else
            hex_0 = (in[i] - 'a') + 10;
        if (in[i + 1] < 'a')
            hex_1 = (in[i + 1] - '0');
        else
            hex_1 = (in[i + 1] - 'a') + 10;
        *pout = ((hex_0 << 4) & 0xF0) | (hex_1 & 0xF);
    }
}

/**
 * \brief Converts bin array to hex string.
 *
 * @param in input binary arrar
 * @param inze size of input
 * @param outStr output hex string
 * @return void
 **/
static void binToHexstr(unsigned char* in, int32_t insz, std::string& outStr) {
    unsigned char* pin = in;
    const char* hex = "0123456789abcdef";
    for (; pin < (in + insz); pin++) {
        outStr = outStr + hex[(*pin >> 4) & 0xF];
        outStr = outStr + hex[*pin & 0xF];
    }
}

/**
 * \brief Allocates compute unit with a device, cu, and channel given a
 * kernel name or alias or both and request load (1 - 100). This function also
 * provides the xclbin and kernel plugin loaded on the device.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of requested cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 *             devExcl: request exclusive device usage for this client.
 *             requestLoad: request load (1 - 100).
 *             poolId: request to allocate cu from specified resource pool
 * @param cuRes the cu resource.
 *             xclbinFileName: xclbin (path and name) attached to this device.
 *             kernelPluginFileName: kernel plugin (only name) attached to this device.
 *             kernelName: the kernel name of allocated cu.
 *             kernelAlias: the name alias of allocated cu.
 *             instanceName: the instance name of allocated cu.
 *             cuName: the name of allocated cu (kernelName:instanceName).
 *             uuid: uuid of the loaded xclbin file.
 *             deviceId: device id of this cu.
 *             cuId: cu id of this cu.
 *             channelId: channel id of this cu.
 *             cuType: type of cu, hardware kernel or soft kernel.
 *             allocServiceId: service id for this cu allocation.
 *             channelLoad: allocated load of this cu (1 - 100).
 *             poolId: id of the cu pool this cu comes from, the system default pool id is 0.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuAlloc(xrmContext context, xrmCuProperty* cuProp, xrmCuResource* cuRes) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL || cuRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu properties or resource pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): wrong request load: %d", __func__, cuProp->requestLoad);
        return (XRM_ERROR_INVALID);
    }

    memset(cuRes, 0, sizeof(xrmCuResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuAllocTree;
    cuAllocTree.put("request.name", "cuAlloc");
    cuAllocTree.put("request.requestId", 1);
    cuAllocTree.put("request.parameters.echoClientId", "echo");
    cuAllocTree.put("request.parameters.clientId", ctx->xrmClientId);
    /* use either kernel name or alias or both to identity the kernel */
    cuAllocTree.put("request.parameters.kernelName", cuProp->kernelName);
    cuAllocTree.put("request.parameters.kernelAlias", cuProp->kernelAlias);
    if (cuProp->devExcl == false)
        cuAllocTree.put("request.parameters.devExcl", 0);
    else
        cuAllocTree.put("request.parameters.devExcl", 1);
    cuAllocTree.put("request.parameters.requestLoad", cuProp->requestLoad);
    cuAllocTree.put("request.parameters.poolId", cuProp->poolId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuAllocTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    int32_t ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName");
        strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto uuidStr = rspTree.get<std::string>("response.data.uuidStr");
        hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
        auto kernelPluginFileName = rspTree.get<std::string>("response.data.kernelPluginFileName");
        strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto kernelName = rspTree.get<std::string>("response.data.kernelName");
        strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias");
        strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
        auto instanceName = rspTree.get<std::string>("response.data.instanceName");
        strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto cuName = rspTree.get<std::string>("response.data.cuName");
        strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
        cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId");
        cuRes->cuId = rspTree.get<int32_t>("response.data.cuId");
        cuRes->channelId = rspTree.get<int32_t>("response.data.channelId");
        auto cuType = rspTree.get<int32_t>("response.data.cuType");
        cuRes->cuType = (xrmCuType)cuType;
        cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId");
        cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad");
        cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr");
        cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId");
        cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType");
        cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize");
        cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr");
        cuRes->poolId = rspTree.get<uint64_t>("response.data.poolId");
    }
    return (ret);
}

/**
 * \brief Allocates a list of compute unit resource given a list of
 * kernels's property with kernel name or alias or both and request load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuListProp the property of cu list.
 *             cuProps: cu prop list to fill kernelName, devExcl and requestLoad, starting from cuProps[0], no hole.
 *             cuNum: request number of cu in this list.
 *             sameDevice: request this list of cu from same device.
 * @param cuListRes the cu list resource.
 *             cuResources: cu resource list to fill the allocated cus infor, starting from cuResources[0], no hole.
 *             cuNum: allocated cu number in this list.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuListAlloc(xrmContext context, xrmCuListProperty* cuListProp, xrmCuListResource* cuListRes) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuListProp == NULL || cuListRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu list properties or resource pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    memset(cuListRes, 0, sizeof(xrmCuListResource));
    if (cuListProp->cuNum <= 0 || cuListProp->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request list prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuListProp->cuNum, XRM_MAX_LIST_CU_NUM);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuListAllocTree;
    cuListAllocTree.put("request.name", "cuListAlloc");
    cuListAllocTree.put("request.requestId", 1);
    cuListAllocTree.put("request.parameters.cuNum", cuListProp->cuNum);
    cuListAllocTree.put("request.parameters.echoClientId", "echo");
    cuListAllocTree.put("request.parameters.clientId", ctx->xrmClientId);
    if (cuListProp->sameDevice == false)
        cuListAllocTree.put("request.parameters.sameDevice", 0);
    else
        cuListAllocTree.put("request.parameters.sameDevice", 1);
    for (i = 0; i < cuListProp->cuNum; i++) {
        cuProp = &cuListProp->cuProps[i];
        if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s cuProps[%d] neither kernel name nor alias are provided",
                   __func__, i);
            return (XRM_ERROR_INVALID);
        }
        if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): cuProps[%d] wrong request load: %d", __func__, i,
                   cuProp->requestLoad);
            return (XRM_ERROR_INVALID);
        }

        cuListAllocTree.put("request.parameters.kernelName" + std::to_string(i), cuProp->kernelName);
        cuListAllocTree.put("request.parameters.kernelAlias" + std::to_string(i), cuProp->kernelAlias);
        if (cuProp->devExcl == false)
            cuListAllocTree.put("request.parameters.devExcl" + std::to_string(i), 0);
        else
            cuListAllocTree.put("request.parameters.devExcl" + std::to_string(i), 1);
        cuListAllocTree.put("request.parameters.requestLoad" + std::to_string(i), cuProp->requestLoad);
        cuListAllocTree.put("request.parameters.poolId" + std::to_string(i), cuProp->poolId);
    }

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuListAllocTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        cuListRes->cuNum = rspTree.get<int32_t>("response.data.cuNum");
        for (i = 0; i < cuListRes->cuNum; i++) {
            cuRes = &cuListRes->cuResources[i];

            auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName" + std::to_string(i));
            strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto uuidStr = rspTree.get<std::string>("response.data.uuidStr" + std::to_string(i));
            hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
            auto kernelPluginFileName =
                rspTree.get<std::string>("response.data.kernelPluginFileName" + std::to_string(i));
            strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelName = rspTree.get<std::string>("response.data.kernelName" + std::to_string(i));
            strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias" + std::to_string(i));
            strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
            auto instanceName = rspTree.get<std::string>("response.data.instanceName" + std::to_string(i));
            strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto cuName = rspTree.get<std::string>("response.data.cuName" + std::to_string(i));
            strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
            cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId" + std::to_string(i));
            cuRes->cuId = rspTree.get<int32_t>("response.data.cuId" + std::to_string(i));
            cuRes->channelId = rspTree.get<int32_t>("response.data.channelId" + std::to_string(i));
            auto cuType = rspTree.get<int32_t>("response.data.cuType" + std::to_string(i));
            cuRes->cuType = (xrmCuType)cuType;
            cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId" + std::to_string(i));
            cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad" + std::to_string(i));
            cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr" + std::to_string(i));
            cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId" + std::to_string(i));
            cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType" + std::to_string(i));
            cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize" + std::to_string(i));
            cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr" + std::to_string(i));
            cuRes->poolId = rspTree.get<int32_t>("response.data.poolId" + std::to_string(i));
        }
    }
    return (ret);
}

/**
 * \brief Declares user defined cu group type given the specified
 * kernels's property with cu name (kernelName:instanceName) and request load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param udfCuGroupProp the property of user defined cu group.
 *             optionUdfCuListProps[]: option cu list property array starting from optionCuListProps[0], no hole.
 *             optionUdfCuListNum: number of option user defined cu list.
 * @param udfCuGroupName unique user defined cu group name for the new group type declaration
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmUdfCuGroupDeclare(xrmContext context, xrmUdfCuGroupProperty* udfCuGroupProp, char* udfCuGroupName) {
    int32_t ret = XRM_ERROR;
    int32_t cuListIdx;
    xrmUdfCuProperty* udfCuProp;
    xrmUdfCuListProperty* udfCuListProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || udfCuGroupProp == NULL || udfCuGroupName == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, udf cu group property or name pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): udf cu group name is not provided\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (udfCuGroupProp->optionUdfCuListNum <= 0 ||
        udfCuGroupProp->optionUdfCuListNum > XRM_MAX_UDF_CU_GROUP_OPTION_LIST_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): optionUdfCuListNum : %d out of range\n", __func__,
               udfCuGroupProp->optionUdfCuListNum);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree groupDeclareTree;
    groupDeclareTree.put("request.name", "udfCuGroupDeclare");
    groupDeclareTree.put("request.requestId", 1);
    groupDeclareTree.put("request.parameters.udfCuGroupName", udfCuGroupName);
    groupDeclareTree.put("request.parameters.optionUdfCuListNum", udfCuGroupProp->optionUdfCuListNum);
    for (cuListIdx = 0; cuListIdx < udfCuGroupProp->optionUdfCuListNum; cuListIdx++) {
        udfCuListProp = &udfCuGroupProp->optionUdfCuListProps[cuListIdx];
        std::string udfCuListStr = "request.parameters.optionUdfCuListProps" + std::to_string(cuListIdx);
        if (udfCuListProp->sameDevice == false)
            groupDeclareTree.put(udfCuListStr + ".sameDevice", 0);
        else
            groupDeclareTree.put(udfCuListStr + ".sameDevice", 1);
        groupDeclareTree.put(udfCuListStr + ".cuNum", udfCuListProp->cuNum);
        for (int32_t i = 0; i < udfCuListProp->cuNum; i++) {
            udfCuProp = &udfCuListProp->udfCuProps[i];
            if (udfCuProp->cuName[0] == '\0') {
                xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s udfCuProps[%d] cu name is NOT provided", __func__, i);
                return (XRM_ERROR_INVALID);
            }
            if (udfCuProp->requestLoad <= 0 || udfCuProp->requestLoad > 100) {
                xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): udfCuProps[%d] wrong request load: %d", __func__, i,
                       udfCuProp->requestLoad);
                return (XRM_ERROR_INVALID);
            }

            groupDeclareTree.put(udfCuListStr + ".cuName" + std::to_string(i), udfCuProp->cuName);
            if (udfCuProp->devExcl == false)
                groupDeclareTree.put(udfCuListStr + ".devExcl" + std::to_string(i), 0);
            else
                groupDeclareTree.put(udfCuListStr + ".devExcl" + std::to_string(i), 1);
            groupDeclareTree.put(udfCuListStr + ".requestLoad" + std::to_string(i), udfCuProp->requestLoad);
        }
    }

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, groupDeclareTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    return (ret);
}

/**
 * \brief Undeclares user defined cu group type given the specified
 * group name.
 *
 * @param context the context created through xrmCreateContext()
 * @param udfCuGroupName user defined cu group name for the group type undeclaration
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmUdfCuGroupUndeclare(xrmContext context, char* udfCuGroupName) {
    int32_t ret = XRM_ERROR;
    int32_t cuListIdx;
    xrmUdfCuProperty* udfCuProp;
    xrmUdfCuListProperty* udfCuListProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || udfCuGroupName == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or udf cu group name pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): udf cu group name is not provided\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree groupUndeclareTree;
    groupUndeclareTree.put("request.name", "udfCuGroupUndeclare");
    groupUndeclareTree.put("request.requestId", 1);
    groupUndeclareTree.put("request.parameters.udfCuGroupName", udfCuGroupName);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, groupUndeclareTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    return (ret);
}

/**
 * \brief Allocates a group of compute unit resource given a user defined group of
 * kernels's property with cu name (kernelName:instanceName) and request load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuGroupProp the property of cu group.
 *             udfCuGroupName: user defined cu group type name.
 *             poolId: id of the cu pool this group CUs come from, the system default pool id is 0.
 * @param cuGroupRes the cu group resource.
 *             cuResources: cu resource group to fill the allocated cus infor, starting from cuResources[0], no hole.
 *             cuNum: allocated cu number in this group.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuGroupAlloc(xrmContext context, xrmCuGroupProperty* cuGroupProp, xrmCuGroupResource* cuGroupRes) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuGroupProp == NULL || cuGroupRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu group property or resource pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuGroupProp->udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: udfCuGroupName is not provided.\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    memset(cuGroupRes, 0, sizeof(xrmCuGroupResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuGroupAllocTree;
    cuGroupAllocTree.put("request.name", "cuGroupAlloc");
    cuGroupAllocTree.put("request.requestId", 1);
    cuGroupAllocTree.put("request.parameters.udfCuGroupName", cuGroupProp->udfCuGroupName);
    cuGroupAllocTree.put("request.parameters.poolId", cuGroupProp->poolId);
    cuGroupAllocTree.put("request.parameters.echoClientId", "echo");
    cuGroupAllocTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuGroupAllocTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        cuGroupRes->cuNum = rspTree.get<int32_t>("response.data.cuNum");
        for (i = 0; i < cuGroupRes->cuNum; i++) {
            cuRes = &cuGroupRes->cuResources[i];

            auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName" + std::to_string(i));
            strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto uuidStr = rspTree.get<std::string>("response.data.uuidStr" + std::to_string(i));
            hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
            auto kernelPluginFileName =
                rspTree.get<std::string>("response.data.kernelPluginFileName" + std::to_string(i));
            strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelName = rspTree.get<std::string>("response.data.kernelName" + std::to_string(i));
            strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias" + std::to_string(i));
            strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
            auto instanceName = rspTree.get<std::string>("response.data.instanceName" + std::to_string(i));
            strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto cuName = rspTree.get<std::string>("response.data.cuName" + std::to_string(i));
            strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
            cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId" + std::to_string(i));
            cuRes->cuId = rspTree.get<int32_t>("response.data.cuId" + std::to_string(i));
            cuRes->channelId = rspTree.get<int32_t>("response.data.channelId" + std::to_string(i));
            auto cuType = rspTree.get<int32_t>("response.data.cuType" + std::to_string(i));
            cuRes->cuType = (xrmCuType)cuType;
            cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId" + std::to_string(i));
            cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad" + std::to_string(i));
            cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr" + std::to_string(i));
            cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId" + std::to_string(i));
            cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType" + std::to_string(i));
            cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize" + std::to_string(i));
            cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr" + std::to_string(i));
            cuRes->poolId = rspTree.get<int32_t>("response.data.poolId" + std::to_string(i));
        }
    }
    return (ret);
}

/**
 * \brief Retrieves the maximum capacity associated with a resource
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 * @return uint64_t, the max capacity of the cu (> 0) or 0 if cu is not existing in system or max capacity
 *          is not described.
 */
uint64_t xrmCuGetMaxCapacity(xrmContext context, xrmCuProperty* cuProp) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "context or cu properties pointer is NULL\n");
        return (0);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (0);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (0);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuGetMaxCapacityTree;
    cuGetMaxCapacityTree.put("request.name", "cuGetMaxCapacity");
    cuGetMaxCapacityTree.put("request.requestId", 1);
    cuGetMaxCapacityTree.put("request.parameters.kernelName", cuProp->kernelName);
    cuGetMaxCapacityTree.put("request.parameters.kernelAlias", cuProp->kernelAlias);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuGetMaxCapacityTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (0);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto maxCapacity = rspTree.get<uint64_t>("response.status.value");
    return (maxCapacity);
}

/**
 * \brief To check the status of specified cu resource
 *
 * @param context the context created through xrmCreateContext()
 * @param cuRes the cu resource.
 *             deviceId: device id of this cu.
 *             cuId: cu id of this cu.
 *             channelId: channel id of this cu.
 *             cuType: type of cu, hardware kernel or soft kernel.
 *             allocServiceId: service id for this cu allocation.
 * @param cuStat the status of cu.
 *             isBusy: the cu is busy or not.
 *             usedLoad: allocated load on this cu.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuCheckStatus(xrmContext context, xrmCuResource* cuRes, xrmCuStat* cuStat) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuRes == NULL || cuStat == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s: context, cu resource or stat pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s: wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuRes->allocServiceId == 0) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s: invalid allocServiceId 0\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    memset(cuStat, 0, sizeof(xrmCuStat));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuCheckStatusTree;
    cuCheckStatusTree.put("request.name", "cuCheckStatus");
    cuCheckStatusTree.put("request.requestId", 1);
    cuCheckStatusTree.put("request.parameters.deviceId", cuRes->deviceId);
    cuCheckStatusTree.put("request.parameters.cuId", cuRes->cuId);
    cuCheckStatusTree.put("request.parameters.channelId", cuRes->channelId);
    cuCheckStatusTree.put("request.parameters.cuType", (int32_t)cuRes->cuType);
    cuCheckStatusTree.put("request.parameters.allocServiceId", cuRes->allocServiceId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuCheckStatusTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        auto isBusy = rspTree.get<int32_t>("response.data.isBusy");
        if (isBusy == 0)
            cuStat->isBusy = false;
        else
            cuStat->isBusy = true;
        cuStat->usedLoad = rspTree.get<int32_t>("response.data.usedLoad");
    }
    return (ret);
}

/**
 * \brief Releases a previously allocated resource
 *
 * @param context the context created through xrmCreateContext()
 * @param cuRes the cu resource.
 *             xclbinFileName: xclbin (path and name) attached to this device.
 *             kernelPluginFileName: kernel plugin (only name) attached to this device.
 *             kernelName: the kernel name of allocated cu.
 *             kernelAlias: the name alias of allocated cu.
 *             instanceName: the instance name of allocated cu.
 *             cuName: the name of allocated cu (kernelName:instanceName).
 *             uuid: uuid of the loaded xclbin file.
 *             deviceId: device id of this cu.
 *             cuId: cu id of this cu.
 *             channelId: channel id of this cu.
 *             cuType: type of cu, hardware kernel or soft kernel.
 *             allocServiceId: service id for this cu allocation.
 *             channelLoad: allocated load of this cu (1 - 100).
 *             poolId: id of the cu pool this cu comes from, the system default pool id is 0.
 * @return bool, true on success or false on fail
 */
bool xrmCuRelease(xrmContext context, xrmCuResource* cuRes) {
    bool ret = false;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu resource pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree xrmCuRelease;
    xrmCuRelease.put("request.name", "cuRelease");
    xrmCuRelease.put("request.requestId", 1);
    xrmCuRelease.put("request.parameters.deviceId", cuRes->deviceId);
    xrmCuRelease.put("request.parameters.cuId", cuRes->cuId);
    xrmCuRelease.put("request.parameters.channelId", cuRes->channelId);
    xrmCuRelease.put("request.parameters.cuType", (int32_t)cuRes->cuType);
    xrmCuRelease.put("request.parameters.allocServiceId", cuRes->allocServiceId);
    xrmCuRelease.put("request.parameters.echoClientId", "echo");
    xrmCuRelease.put("request.parameters.clientId", ctx->xrmClientId);
    xrmCuRelease.put("request.parameters.channelLoad", cuRes->channelLoad);
    xrmCuRelease.put("request.parameters.poolId", cuRes->poolId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, xrmCuRelease);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (ret);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        ret = true;
    }
    return (ret);
}

/**
 * \brief Releases a previously allocated list of resources
 *
 * @param context the context created through xrmCreateContext()
 * @param cuListRes the cu list resource.
 *             cuResources: cu resource list to be released, starting from cuResources[0], no hole.
 *             cuNum: number of cu in this list.
 * @return bool, true on success or false on fail
 */
bool xrmCuListRelease(xrmContext context, xrmCuListResource* cuListRes) {
    bool ret = false;
    int32_t i;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuListRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu list resource pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if (cuListRes->cuNum < 0 || cuListRes->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): list resource cuNum is %d, out of range 0 - %d.\n", __func__,
               cuListRes->cuNum, XRM_MAX_LIST_CU_NUM);
        return (ret);
    }
    if (cuListRes->cuNum == 0) {
        /* Nothing to release */
        return (ret);
    }

    /* will release all the resource */
    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuListReleaseTree;
    cuListReleaseTree.put("request.name", "cuListRelease");
    cuListReleaseTree.put("request.requestId", 1);
    cuListReleaseTree.put("request.parameters.cuNum", cuListRes->cuNum);
    cuListReleaseTree.put("request.parameters.echoClientId", "echo");
    cuListReleaseTree.put("request.parameters.clientId", ctx->xrmClientId);
    for (i = 0; i < cuListRes->cuNum; i++) {
        cuRes = &cuListRes->cuResources[i];
        cuListReleaseTree.put("request.parameters.deviceId" + std::to_string(i), cuRes->deviceId);
        cuListReleaseTree.put("request.parameters.cuId" + std::to_string(i), cuRes->cuId);
        cuListReleaseTree.put("request.parameters.channelId" + std::to_string(i), cuRes->channelId);
        cuListReleaseTree.put("request.parameters.cuType" + std::to_string(i), (int32_t)cuRes->cuType);
        cuListReleaseTree.put("request.parameters.allocServiceId" + std::to_string(i), cuRes->allocServiceId);
        cuListReleaseTree.put("request.parameters.channelLoad" + std::to_string(i), cuRes->channelLoad);
        cuListReleaseTree.put("request.parameters.poolId" + std::to_string(i), cuRes->poolId);
    }
    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuListReleaseTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (ret);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        ret = true;
    }

    return (ret);
}

/**
 * \brief Releases a previously allocated group of resources
 *
 * @param context the context created through xrmCreateContext()
 * @param cuGroupRes cu group resource.
 *             cuResources: cu resource group to be released, starting from cuResources[0], no hole.
 *             cuNum: number of cu in this group.
 * @return bool, true on success or false on fail
 */
bool xrmCuGroupRelease(xrmContext context, xrmCuGroupResource* cuGroupRes) {
    bool ret = false;
    int32_t i;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuGroupRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu group resource pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if (cuGroupRes->cuNum < 0 || cuGroupRes->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): group resource cuNum is %d, out of range 0 - %d.\n", __func__,
               cuGroupRes->cuNum, XRM_MAX_LIST_CU_NUM);
        return (ret);
    }
    if (cuGroupRes->cuNum == 0) {
        /* Nothing to release */
        return (ret);
    }

    /* will release all the resource */
    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuGroupReleaseTree;
    cuGroupReleaseTree.put("request.name", "cuGroupRelease");
    cuGroupReleaseTree.put("request.requestId", 1);
    cuGroupReleaseTree.put("request.parameters.cuNum", cuGroupRes->cuNum);
    cuGroupReleaseTree.put("request.parameters.echoClientId", "echo");
    cuGroupReleaseTree.put("request.parameters.clientId", ctx->xrmClientId);
    for (i = 0; i < cuGroupRes->cuNum; i++) {
        cuRes = &cuGroupRes->cuResources[i];
        cuGroupReleaseTree.put("request.parameters.deviceId" + std::to_string(i), cuRes->deviceId);
        cuGroupReleaseTree.put("request.parameters.cuId" + std::to_string(i), cuRes->cuId);
        cuGroupReleaseTree.put("request.parameters.channelId" + std::to_string(i), cuRes->channelId);
        cuGroupReleaseTree.put("request.parameters.cuType" + std::to_string(i), (int32_t)cuRes->cuType);
        cuGroupReleaseTree.put("request.parameters.allocServiceId" + std::to_string(i), cuRes->allocServiceId);
        cuGroupReleaseTree.put("request.parameters.channelLoad" + std::to_string(i), cuRes->channelLoad);
        cuGroupReleaseTree.put("request.parameters.poolId" + std::to_string(i), cuRes->poolId);
    }
    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuGroupReleaseTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (ret);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        ret = true;
    }

    return (ret);
}

/**
 * \brief Querys the compute unit resource given the allocation service id.
 *
 * @param context the context created through xrmCreateContext()
 * @param allocQuery the allocate query information.
 *             allocServiceId: the service id returned from allocation.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 * @param cuListRes cu list resource.
 *             cuListRes: cu resource list to fill the allocated cus infor, starting from cuResources[0], no hole.
 *             cuNum: cu number in this list.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmAllocationQuery(xrmContext context, xrmAllocationQueryInfo* allocQuery, xrmCuListResource* cuListRes) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || allocQuery == NULL || cuListRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, allocation query or cu list pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (allocQuery->allocServiceId == 0) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s invalid allocServiceId: 0 ", __func__);
        return (XRM_ERROR_INVALID);
    }
    memset(cuListRes, 0, sizeof(xrmCuListResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree allocQueryTree;
    allocQueryTree.put("request.name", "allocationQuery");
    allocQueryTree.put("request.requestId", 1);
    allocQueryTree.put("request.parameters.allocServiceId", allocQuery->allocServiceId);
    allocQueryTree.put("request.parameters.kernelName", allocQuery->kernelName);
    allocQueryTree.put("request.parameters.kernelAlias", allocQuery->kernelAlias);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, allocQueryTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        cuListRes->cuNum = rspTree.get<int32_t>("response.data.cuNum");
        for (i = 0; i < cuListRes->cuNum; i++) {
            cuRes = &cuListRes->cuResources[i];

            auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName" + std::to_string(i));
            strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto uuidStr = rspTree.get<std::string>("response.data.uuidStr" + std::to_string(i));
            hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
            auto kernelPluginFileName =
                rspTree.get<std::string>("response.data.kernelPluginFileName" + std::to_string(i));
            strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelName = rspTree.get<std::string>("response.data.kernelName" + std::to_string(i));
            strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias" + std::to_string(i));
            strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
            auto instanceName = rspTree.get<std::string>("response.data.instanceName" + std::to_string(i));
            strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto cuName = rspTree.get<std::string>("response.data.cuName" + std::to_string(i));
            strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
            cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId" + std::to_string(i));
            cuRes->cuId = rspTree.get<int32_t>("response.data.cuId" + std::to_string(i));
            cuRes->channelId = rspTree.get<int32_t>("response.data.channelId" + std::to_string(i));
            auto cuType = rspTree.get<int32_t>("response.data.cuType" + std::to_string(i));
            cuRes->cuType = (xrmCuType)cuType;
            cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId" + std::to_string(i));
            cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr" + std::to_string(i));
            cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId" + std::to_string(i));
            cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType" + std::to_string(i));
            cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize" + std::to_string(i));
            cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr" + std::to_string(i));
            cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad" + std::to_string(i));
            cuRes->poolId = rspTree.get<int32_t>("response.data.poolId" + std::to_string(i));
        }
    }
    return (ret);
}

/**
 * \brief To check whether the cu exists on the system given
 * the kernels's property with kernel name or alias or both.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 * @return bool, true on existing or false on NOT existing.
 */
bool xrmIsCuExisting(xrmContext context, xrmCuProperty* cuProp) {
    bool ret = false;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu property pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree isCuExistingTree;
    isCuExistingTree.put("request.name", "isCuExisting");
    isCuExistingTree.put("request.requestId", 1);
    /* use either kernel name or alias or both to identity the kernel */
    isCuExistingTree.put("request.parameters.kernelName", cuProp->kernelName);
    isCuExistingTree.put("request.parameters.kernelAlias", cuProp->kernelAlias);
    isCuExistingTree.put("request.parameters.echoClientId", "echo");
    isCuExistingTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, isCuExistingTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        auto isCuExisting = rspTree.get<int32_t>("response.data.isCuExisting");
        if (isCuExisting == 1)
            ret = true;
    }
    return (ret);
}

/**
 * \brief To check whether the cu list exists on the system given
 * a list of kernels's property with kernel name or alias or both.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuListProp the property of cu list.
 *             cuProps: cu prop list to fill kernelName and kernelAlias, starting from cuProps[0], no hole
 *             cuNum: request number of cu in this list.
 *             sameDevice: request this list of cu from same device.
 * @return bool, true on existing or false on NOT existing.
 */
bool xrmIsCuListExisting(xrmContext context, xrmCuListProperty* cuListProp) {
    bool ret = false;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuListProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu list properties pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if (cuListProp->cuNum <= 0 || cuListProp->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request list prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuListProp->cuNum, XRM_MAX_LIST_CU_NUM);
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree isCuListExistingTree;
    isCuListExistingTree.put("request.name", "isCuListExisting");
    isCuListExistingTree.put("request.requestId", 1);
    isCuListExistingTree.put("request.parameters.cuNum", cuListProp->cuNum);
    isCuListExistingTree.put("request.parameters.echoClientId", "echo");
    isCuListExistingTree.put("request.parameters.clientId", ctx->xrmClientId);
    if (cuListProp->sameDevice == false)
        isCuListExistingTree.put("request.parameters.sameDevice", 0);
    else
        isCuListExistingTree.put("request.parameters.sameDevice", 1);
    for (i = 0; i < cuListProp->cuNum; i++) {
        cuProp = &cuListProp->cuProps[i];
        if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s cuProp[%d] neither kernel name nor alias are provided",
                   __func__, i);
            return (ret);
        }

        isCuListExistingTree.put("request.parameters.kernelName" + std::to_string(i), cuProp->kernelName);
        isCuListExistingTree.put("request.parameters.kernelAlias" + std::to_string(i), cuProp->kernelAlias);
    }

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, isCuListExistingTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        auto isCuListExisting = rspTree.get<int32_t>("response.data.isCuListExisting");
        if (isCuListExisting == 1)
            ret = true;
    }
    return (ret);
}

/**
 * \brief To check whether the cu group exists on the system given a user
 * defined group of kernels's property with cu name (kernelName:instanceName).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuGroupProp the property of cu group.
 *             udfCuGroupName: user defined cu group type name.
 * @return bool, true on existing or false on NOT existing.
 */
bool xrmIsCuGroupExisting(xrmContext context, xrmCuGroupProperty* cuGroupProp) {
    bool ret = false;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuGroupProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu pool properties pointer is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if (cuGroupProp->udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: udfCuGroupName is not provided.\n", __func__);
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree checkCuGroupTree;
    checkCuGroupTree.put("request.name", "isCuGroupExisting");
    checkCuGroupTree.put("request.requestId", 1);
    checkCuGroupTree.put("request.parameters.udfCuGroupName", cuGroupProp->udfCuGroupName);
    checkCuGroupTree.put("request.parameters.echoClientId", "echo");
    checkCuGroupTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, checkCuGroupTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        auto isCuGroupExisting = rspTree.get<int32_t>("response.data.isCuGroupExisting");
        if (isCuGroupExisting == 1)
            ret = true;
    }
    return (ret);
}

/**
 * \brief To check the available cu num on the system given
 * the kernels's property with kernel name or alias or both and request
 * load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 *             devExcl: request exclusive device usage for this client.
 *             requestLoad: request load (1 - 100).
 *             poolId: request to allocate cu from specified resource pool.
 * @return int32_t, available cu num (>= 0) on success or appropriate error number (< 0), if available
 *          cu number is >= XRM_MAX_AVAILABLE_CU_NUM, will only return XRM_MAX_AVAILABLE_CU_NUM.
 */
int32_t xrmCheckCuAvailableNum(xrmContext context, xrmCuProperty* cuProp) {
    int32_t ret = XRM_ERROR;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu property pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): wrong request load: %d", __func__, cuProp->requestLoad);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree checkCuAvailableTree;
    checkCuAvailableTree.put("request.name", "checkCuAvailableNum");
    checkCuAvailableTree.put("request.requestId", 1);
    /* use either kernel name or alias or both to identity the kernel */
    checkCuAvailableTree.put("request.parameters.kernelName", cuProp->kernelName);
    checkCuAvailableTree.put("request.parameters.kernelAlias", cuProp->kernelAlias);
    if (cuProp->devExcl == false)
        checkCuAvailableTree.put("request.parameters.devExcl", 0);
    else
        checkCuAvailableTree.put("request.parameters.devExcl", 1);
    checkCuAvailableTree.put("request.parameters.requestLoad", cuProp->requestLoad);
    checkCuAvailableTree.put("request.parameters.echoClientId", "echo");
    checkCuAvailableTree.put("request.parameters.clientId", ctx->xrmClientId);
    checkCuAvailableTree.put("request.parameters.poolId", cuProp->poolId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, checkCuAvailableTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        ret = rspTree.get<int32_t>("response.data.availableCuNum");
    }
    return (ret);
}

/**
 * \brief To check the available cu list num on the system given
 * a list of kernels's property with kernel name or alias or both and request
 * load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuListProp the property of cu list.
 *             cuProps: cu prop list to fill kernelName, devExcl and requestLoad, starting from cuProps[0], no hole
 *             cuNum: request number of cu in this list.
 *             sameDevice: request this list of cu from same device.
 * @return int32_t, available cu list num (>= 0) on success or appropriate error number (< 0), if available
 *          cu list number is >= XRM_MAX_AVAILABLE_LIST_NUM, will only return XRM_MAX_AVAILABLE_LIST_NUM.
 */
int32_t xrmCheckCuListAvailableNum(xrmContext context, xrmCuListProperty* cuListProp) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuListProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu list properties pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuListProp->cuNum <= 0 || cuListProp->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request list prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuListProp->cuNum, XRM_MAX_LIST_CU_NUM);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree checkCuListAvailableNumTree;
    checkCuListAvailableNumTree.put("request.name", "checkCuListAvailableNum");
    checkCuListAvailableNumTree.put("request.requestId", 1);
    checkCuListAvailableNumTree.put("request.parameters.cuNum", cuListProp->cuNum);
    checkCuListAvailableNumTree.put("request.parameters.echoClientId", "echo");
    checkCuListAvailableNumTree.put("request.parameters.clientId", ctx->xrmClientId);
    if (cuListProp->sameDevice == false)
        checkCuListAvailableNumTree.put("request.parameters.sameDevice", 0);
    else
        checkCuListAvailableNumTree.put("request.parameters.sameDevice", 1);
    for (i = 0; i < cuListProp->cuNum; i++) {
        cuProp = &cuListProp->cuProps[i];
        if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s cuProp[%d] neither kernel name nor alias are provided",
                   __func__, i);
            return (XRM_ERROR_INVALID);
        }
        if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): cuProp[%d] wrong request load: %d", __func__, i,
                   cuProp->requestLoad);
            return (XRM_ERROR_INVALID);
        }

        checkCuListAvailableNumTree.put("request.parameters.kernelName" + std::to_string(i), cuProp->kernelName);
        checkCuListAvailableNumTree.put("request.parameters.kernelAlias" + std::to_string(i), cuProp->kernelAlias);
        if (cuProp->devExcl == false)
            checkCuListAvailableNumTree.put("request.parameters.devExcl" + std::to_string(i), 0);
        else
            checkCuListAvailableNumTree.put("request.parameters.devExcl" + std::to_string(i), 1);
        checkCuListAvailableNumTree.put("request.parameters.requestLoad" + std::to_string(i), cuProp->requestLoad);
        checkCuListAvailableNumTree.put("request.parameters.poolId" + std::to_string(i), cuProp->poolId);
    }

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, checkCuListAvailableNumTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        ret = rspTree.get<int32_t>("response.data.availableListNum");
    }
    return (ret);
}

/**
 * \brief To check the available group number of compute unit resource given a user
 * defined group of kernels's property with cu name (kernelName:instanceName) and request load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuGroupProp the property of cu group.
 *             udfCuGroupName: user defined cu group type name.
 *             poolId: id of the cu pool this group CUs come from, the system default pool id is 0.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCheckCuGroupAvailableNum(xrmContext context, xrmCuGroupProperty* cuGroupProp) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuGroupProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu pool properties pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuGroupProp->udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: udfCuGroupName is not provided.\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree checkCuGroupTree;
    checkCuGroupTree.put("request.name", "checkCuGroupAvailableNum");
    checkCuGroupTree.put("request.requestId", 1);
    checkCuGroupTree.put("request.parameters.udfCuGroupName", cuGroupProp->udfCuGroupName);
    checkCuGroupTree.put("request.parameters.poolId", cuGroupProp->poolId);
    checkCuGroupTree.put("request.parameters.echoClientId", "echo");
    checkCuGroupTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, checkCuGroupTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        ret = rspTree.get<int32_t>("response.data.availableGroupNum");
    }
    return (ret);
}

/**
 * \brief To check the available cu pool num on the system given
 * a pool of kernels's property with kernel name or alias or both and request
 * load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuPoolProp the property of cu pool.
 *             cuListProp: cu list property.
 *             cuListNum: number of cu list in this pool.
 *             xclbinUuid: uuid of xclbin.
 *             xclbinNum: number of xclbin in this pool.
 * @return int32_t, available cu pool num (>= 0) on success or appropriate error number (< 0), if available
 *          cu pool number is >= XRM_MAX_AVAILABLE_POOL_NUM, will only return XRM_MAX_AVAILABLE_POOL_NUM.
 */
int32_t xrmCheckCuPoolAvailableNum(xrmContext context, xrmCuPoolProperty* cuPoolProp) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuPoolProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu pool properties pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->cuListNum < 0) || (cuPoolProp->xclbinNum < 0) ||
        ((cuPoolProp->cuListNum == 0) && (cuPoolProp->xclbinNum == 0))) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: cuListNum is %d, xclbinNum is %d.\n", __func__,
               cuPoolProp->cuListNum, cuPoolProp->xclbinNum);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->cuListNum > 0) &&
        (cuPoolProp->cuListProp.cuNum <= 0 || cuPoolProp->cuListProp.cuNum > XRM_MAX_LIST_CU_NUM)) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request pool prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuPoolProp->cuListProp.cuNum, XRM_MAX_LIST_CU_NUM);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->xclbinNum > 0) && (cuPoolProp->xclbinUuid == 0)) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request pool prop xclbinUuid is not specified.\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree checkCuPoolTree;
    checkCuPoolTree.put("request.name", "checkCuPoolAvailableNum");
    checkCuPoolTree.put("request.requestId", 1);
    checkCuPoolTree.put("request.parameters.cuListNum", cuPoolProp->cuListNum);
    checkCuPoolTree.put("request.parameters.cuNum", cuPoolProp->cuListProp.cuNum);
    checkCuPoolTree.put("request.parameters.echoClientId", "echo");
    checkCuPoolTree.put("request.parameters.clientId", ctx->xrmClientId);
    if (cuPoolProp->cuListProp.sameDevice == false)
        checkCuPoolTree.put("request.parameters.sameDevice", 0);
    else
        checkCuPoolTree.put("request.parameters.sameDevice", 1);
    for (i = 0; i < cuPoolProp->cuListProp.cuNum; i++) {
        cuProp = &cuPoolProp->cuListProp.cuProps[i];
        if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s cuProp[%d] neither kernel name nor alias are provided",
                   __func__, i);
            return (XRM_ERROR_INVALID);
        }
        if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): cuProp[%d] wrong request load: %d", __func__, i,
                   cuProp->requestLoad);
            return (XRM_ERROR_INVALID);
        }

        checkCuPoolTree.put("request.parameters.kernelName" + std::to_string(i), cuProp->kernelName);
        checkCuPoolTree.put("request.parameters.kernelAlias" + std::to_string(i), cuProp->kernelAlias);
        if (cuProp->devExcl == false)
            checkCuPoolTree.put("request.parameters.devExcl" + std::to_string(i), 0);
        else
            checkCuPoolTree.put("request.parameters.devExcl" + std::to_string(i), 1);
        checkCuPoolTree.put("request.parameters.requestLoad" + std::to_string(i), cuProp->requestLoad);
    }
    std::string uuidStr;
    binToHexstr((unsigned char*)&cuPoolProp->xclbinUuid, sizeof(uuid_t), uuidStr);
    checkCuPoolTree.put("request.parameters.xclbinUuidStr", uuidStr.c_str());
    checkCuPoolTree.put("request.parameters.xclbinNum", cuPoolProp->xclbinNum);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, checkCuPoolTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        ret = rspTree.get<int32_t>("response.data.availablePoolNum");
    }
    return (ret);
}

/**
 * \brief Reserves a pool of compute unit resource given a pool of
 * kernels's property with kernel name or alias or both and request load (1 - 100).
 *
 * @param context the context created through xrmCreateContext()
 * @param cuPoolProp the property of cu pool.
 *             cuListProp: cu prop list to fill kernelName, devExcl and requestLoad etc. information.
 *             cuListNum: request number of such cu list for this pool.
 *             xclbinUuid: request all resource in the xclbin.
 *             xclbinNum: request number of such xclbin for this pool.
 * @return uint64_t, reserve pool id (> 0) or 0 on fail
 */
uint64_t xrmCuPoolReserve(xrmContext context, xrmCuPoolProperty* cuPoolProp) {
    uint64_t reserve_poolId = 0;
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuPoolProp == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu pool properties pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->cuListNum < 0) || (cuPoolProp->xclbinNum < 0) ||
        ((cuPoolProp->cuListNum == 0) && (cuPoolProp->xclbinNum == 0))) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: cuListNum is %d, xclbinNum is %d.\n", __func__,
               cuPoolProp->cuListNum, cuPoolProp->xclbinNum);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->cuListNum > 0) &&
        (cuPoolProp->cuListProp.cuNum <= 0 || cuPoolProp->cuListProp.cuNum > XRM_MAX_LIST_CU_NUM)) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request pool prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuPoolProp->cuListProp.cuNum, XRM_MAX_LIST_CU_NUM);
        return (XRM_ERROR_INVALID);
    }
    if ((cuPoolProp->xclbinNum > 0) && (cuPoolProp->xclbinUuid == 0)) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request pool prop xclbinUuid is not specified.\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuPoolReserveTree;
    cuPoolReserveTree.put("request.name", "cuPoolReserve");
    cuPoolReserveTree.put("request.requestId", 1);
    cuPoolReserveTree.put("request.parameters.echoClientId", "echo");
    cuPoolReserveTree.put("request.parameters.clientId", ctx->xrmClientId);
    cuPoolReserveTree.put("request.parameters.cuListNum", cuPoolProp->cuListNum);
    cuPoolReserveTree.put("request.parameters.cuList.cuNum", cuPoolProp->cuListProp.cuNum);
    if (cuPoolProp->cuListProp.sameDevice == false)
        cuPoolReserveTree.put("request.parameters.cuList.sameDevice", 0);
    else
        cuPoolReserveTree.put("request.parameters.cuList.sameDevice", 1);
    for (i = 0; i < cuPoolProp->cuListProp.cuNum; i++) {
        cuProp = &cuPoolProp->cuListProp.cuProps[i];
        if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s cuProp[%d] neither kernel name nor alias are provided",
                   __func__, i);
            return (reserve_poolId);
        }
        if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
            xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): cuProp[%d] wrong request load: %d", __func__, i,
                   cuProp->requestLoad);
            return (reserve_poolId);
        }

        cuPoolReserveTree.put("request.parameters.cuList.kernelName" + std::to_string(i), cuProp->kernelName);
        cuPoolReserveTree.put("request.parameters.cuList.kernelAlias" + std::to_string(i), cuProp->kernelAlias);
        if (cuProp->devExcl == false)
            cuPoolReserveTree.put("request.parameters.cuList.devExcl" + std::to_string(i), 0);
        else
            cuPoolReserveTree.put("request.parameters.cuList.devExcl" + std::to_string(i), 1);
        cuPoolReserveTree.put("request.parameters.cuList.requestLoad" + std::to_string(i), cuProp->requestLoad);
    }
    std::string uuidStr;
    binToHexstr((unsigned char*)&cuPoolProp->xclbinUuid, sizeof(uuid_t), uuidStr);
    cuPoolReserveTree.put("request.parameters.xclbinUuidStr", uuidStr.c_str());
    cuPoolReserveTree.put("request.parameters.xclbinNum", cuPoolProp->xclbinNum);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuPoolReserveTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (reserve_poolId);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        reserve_poolId = rspTree.get<int64_t>("response.data.poolId");
    } else {
        reserve_poolId = 0;
    }
    return (reserve_poolId);
}

/**
 * \brief Relinquishes a previously reserved pool of resources
 *
 * @param context the context created through xrmCreateContext()
 * @param poolId the reserve pool id
 * @return bool, true on success or false on fail
 */
bool xrmCuPoolRelinquish(xrmContext context, uint64_t poolId) {
    bool ret = false;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context is NULL\n", __func__);
        return (ret);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (ret);
    }
    if (poolId == 0) {
        /* Nothing to relinquish */
        return (ret);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuPoolRelinquishTree;
    cuPoolRelinquishTree.put("request.name", "cuPoolRelinquish");
    cuPoolRelinquishTree.put("request.requestId", 1);
    cuPoolRelinquishTree.put("request.parameters.poolId", poolId);
    cuPoolRelinquishTree.put("request.parameters.echoClientId", "echo");
    cuPoolRelinquishTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuPoolRelinquishTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (ret);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    auto value = rspTree.get<int32_t>("response.status.value");
    if (value == XRM_SUCCESS) {
        ret = true;
    }
    return (ret);
}

/**
 * \brief Querys the compute unit resource given the reservation id.
 * NOTE: The allocServiceId, channelId and channelLoad are NOT valid in the cuPoolRes
 *
 * @param context the context created through xrmCreateContext()
 * @param poolId the reserve pool id
 * @param cuPoolRes the cu pool resource.
 *             cuPoolRes: cu resource pool to fill the allocated cus infor, starting from cuPoolRes[0], no hole.
 *             cuNum: cu number in this pool.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmReservationQuery(xrmContext context, uint64_t poolId, xrmCuPoolResource* cuPoolRes) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuPoolRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context or cu pool pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (poolId == 0) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s invalid reserve pool id: 0 ", __func__);
        return (XRM_ERROR_INVALID);
    }
    memset(cuPoolRes, 0, sizeof(xrmCuPoolResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree reservationQueryTree;
    reservationQueryTree.put("request.name", "reservationQuery");
    reservationQueryTree.put("request.requestId", 1);
    reservationQueryTree.put("request.parameters.poolId", poolId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, reservationQueryTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        cuPoolRes->cuNum = rspTree.get<int32_t>("response.data.cuNum");
        for (i = 0; i < cuPoolRes->cuNum; i++) {
            cuRes = &cuPoolRes->cuResources[i];

            auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName" + std::to_string(i));
            strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto uuidStr = rspTree.get<std::string>("response.data.uuidStr" + std::to_string(i));
            hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
            auto kernelPluginFileName =
                rspTree.get<std::string>("response.data.kernelPluginFileName" + std::to_string(i));
            strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelName = rspTree.get<std::string>("response.data.kernelName" + std::to_string(i));
            strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias" + std::to_string(i));
            strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
            auto instanceName = rspTree.get<std::string>("response.data.instanceName" + std::to_string(i));
            strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto cuName = rspTree.get<std::string>("response.data.cuName" + std::to_string(i));
            strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
            cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId" + std::to_string(i));
            cuRes->cuId = rspTree.get<int32_t>("response.data.cuId" + std::to_string(i));
            cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr" + std::to_string(i));
            cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId" + std::to_string(i));
            cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType" + std::to_string(i));
            cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize" + std::to_string(i));
            cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr" + std::to_string(i));
            auto cuType = rspTree.get<int32_t>("response.data.cuType" + std::to_string(i));
            cuRes->cuType = (xrmCuType)cuType;
            cuRes->poolId = rspTree.get<uint64_t>("response.data.poolId" + std::to_string(i));
        }
    }
    return (ret);
}

/**
 * \brief Execuates the function of one specified xrm plugin.
 *
 * @param context the context created through xrmCreateContext()
 * @param xrmPluginName name of the xrm plugin
 * @param funcId the function id within xrm plugin
 * @param param the parameter struct for xrm plugin function
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmExecPluginFunc(xrmContext context, char* xrmPluginName, uint32_t funcId, xrmPluginFuncParam* param) {
    int32_t ret;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if ((ctx == NULL) || (xrmPluginName == NULL) || (param == NULL)) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, xrmPluginName or param is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (funcId > XRM_MAX_PLUGIN_FUNC_ID) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s xrm plugin function id out of range: function id = %d\n", __func__,
               funcId);
        return (XRM_ERROR_INVALID);
    }

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree execPluginFuncTree;
    execPluginFuncTree.put("request.name", "execXrmPluginFunc");
    execPluginFuncTree.put("request.requestId", 1);
    execPluginFuncTree.put("request.parameters.xrmPluginName", xrmPluginName);
    execPluginFuncTree.put("request.parameters.funcId", funcId);
    execPluginFuncTree.put("request.parameters.input", param->input);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, execPluginFuncTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        auto output = rspTree.get<std::string>("response.data.output");
        strncpy(param->output, output.c_str(), XRM_MAX_PLUGIN_FUNC_PARAM_LEN - 1);
        ret = XRM_SUCCESS;
    }
    return (ret);
}

/**
 * \brief Allocates compute unit with a device, cu, and channel given a
 * kernel name or alias or both and request load (1 - 100). This function also
 * provides the xclbin and kernel plugin loaded on the device. If required CU is not
 * available, this function will try to load the xclbin to one device and do the
 * allocation again.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 *             devExcl: request exclusive device usage for this client.
 *             requestLoad: request load (1 - 100).
 *             poolId: request to allocate cu from specified resource pool.
 * @param xclbinFileName xclbin file (full path and name)
 * @param cuRes cu resource.
 *             xclbinFileName: xclbin (path and name) attached to this device.
 *             kernelPluginFileName: kernel plugin (only name) attached to this device.
 *             kernelName: the kernel name of allocated cu.
 *             kernelAlias: the name alias of allocated cu.
 *             instanceName: the instance name of allocated cu.
 *             cuName: the name of allocated cu (kernelName:instanceName).
 *             uuid: uuid of the loaded xclbin file.
 *             deviceId: device id of this cu.
 *             cuId: cu id of this cu.
 *             channelId: channel id of this cu.
 *             cuType: type of cu, hardware kernel or soft kernel.
 *             allocServiceId: service id for this cu allocation.
 *             channelLoad: allocated load of this cu (1 - 100).
 *             poolId: id of the cu pool this cu comes from, the default pool id is 0.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuAllocWithLoad(xrmContext context, xrmCuProperty* cuProp, char* xclbinFileName, xrmCuResource* cuRes) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL || xclbinFileName == NULL || cuRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu properties, xclbin file or resource pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): wrong request load: %d", __func__, cuProp->requestLoad);
        return (XRM_ERROR_INVALID);
    }
    if (xclbinFileName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): xclbin file name is NOT provided", __func__);
        return (XRM_ERROR_INVALID);
    }

    memset(cuRes, 0, sizeof(xrmCuResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree cuAllocWithLoadTree;
    cuAllocWithLoadTree.put("request.name", "cuAllocWithLoad");
    cuAllocWithLoadTree.put("request.requestId", 1);
    cuAllocWithLoadTree.put("request.parameters.echoClientId", "echo");
    cuAllocWithLoadTree.put("request.parameters.clientId", ctx->xrmClientId);
    /* use either kernel name or alias or both to identity the kernel */
    cuAllocWithLoadTree.put("request.parameters.kernelName", cuProp->kernelName);
    cuAllocWithLoadTree.put("request.parameters.kernelAlias", cuProp->kernelAlias);
    if (cuProp->devExcl == false)
        cuAllocWithLoadTree.put("request.parameters.devExcl", 0);
    else
        cuAllocWithLoadTree.put("request.parameters.devExcl", 1);
    cuAllocWithLoadTree.put("request.parameters.requestLoad", cuProp->requestLoad);
    /* this requested cu will only come from default pool */
    cuAllocWithLoadTree.put("request.parameters.poolId", 0);
    cuAllocWithLoadTree.put("request.parameters.xclbinFileName", xclbinFileName);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, cuAllocWithLoadTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    int32_t ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName");
        strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto uuidStr = rspTree.get<std::string>("response.data.uuidStr");
        hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
        auto kernelPluginFileName = rspTree.get<std::string>("response.data.kernelPluginFileName");
        strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto kernelName = rspTree.get<std::string>("response.data.kernelName");
        strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias");
        strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
        auto instanceName = rspTree.get<std::string>("response.data.instanceName");
        strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
        auto cuName = rspTree.get<std::string>("response.data.cuName");
        strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
        cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId");
        cuRes->cuId = rspTree.get<int32_t>("response.data.cuId");
        cuRes->channelId = rspTree.get<int32_t>("response.data.channelId");
        auto cuType = rspTree.get<int32_t>("response.data.cuType");
        cuRes->cuType = (xrmCuType)cuType;
        cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId");
        cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad");
        cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr");
        cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId");
        cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType");
        cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize");
        cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr");
        cuRes->poolId = rspTree.get<uint64_t>("response.data.poolId");
    }
    return (ret);
}

/**
 * \brief Loads xclbin to one device, then allocates all CUs from this device.
 *
 * @param context the context created through xrmCreateContext()
 * @param xclbinFileName xclbin file (full path and name)
 * @param cuListRes cu list resource.
 *             cuResources: cu resource list to fill the allocated cus infor, starting from cuResources[0], no hole.
 *             cuNum: allocated cu number in this list.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmLoadAndAllCuAlloc(xrmContext context, char* xclbinFileName, xrmCuListResource* cuListRes) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || xclbinFileName == NULL || cuListRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, xclbin file or resource list pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (xclbinFileName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): xclbin file name is NOT provided", __func__);
        return (XRM_ERROR_INVALID);
    }

    memset(cuListRes, 0, sizeof(xrmCuListResource));

    char jsonRsp[maxLength];
    memset(jsonRsp, 0, maxLength * sizeof(char));
    pt::ptree loadAndAllCuAllocTree;
    loadAndAllCuAllocTree.put("request.name", "loadAndAllCuAlloc");
    loadAndAllCuAllocTree.put("request.requestId", 1);
    loadAndAllCuAllocTree.put("request.parameters.xclbinFileName", xclbinFileName);
    loadAndAllCuAllocTree.put("request.parameters.echoClientId", "echo");
    loadAndAllCuAllocTree.put("request.parameters.clientId", ctx->xrmClientId);

    std::stringstream reqstr;
    boost::property_tree::write_json(reqstr, loadAndAllCuAllocTree);
    if (xrmJsonRequest(context, reqstr.str().c_str(), jsonRsp) != XRM_SUCCESS) return (XRM_ERROR);

    std::stringstream rspstr;
    rspstr << jsonRsp;
    pt::ptree rspTree;
    boost::property_tree::read_json(rspstr, rspTree);

    int32_t ret, i;
    ret = rspTree.get<int32_t>("response.status.value");
    if (ret == XRM_SUCCESS) {
        cuListRes->cuNum = rspTree.get<int32_t>("response.data.cuNum");
        xrmCuResource* cuRes = NULL;
        for (i = 0; i < cuListRes->cuNum; i++) {
            cuRes = &cuListRes->cuResources[i];

            auto xclbinFileName = rspTree.get<std::string>("response.data.xclbinFileName" + std::to_string(i));
            strncpy(cuRes->xclbinFileName, xclbinFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto uuidStr = rspTree.get<std::string>("response.data.uuidStr" + std::to_string(i));
            hexstrToBin(uuidStr, 2 * sizeof(uuid_t), (unsigned char*)cuRes->uuid);
            auto kernelPluginFileName =
                rspTree.get<std::string>("response.data.kernelPluginFileName" + std::to_string(i));
            strncpy(cuRes->kernelPluginFileName, kernelPluginFileName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelName = rspTree.get<std::string>("response.data.kernelName" + std::to_string(i));
            strncpy(cuRes->kernelName, kernelName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto kernelAlias = rspTree.get<std::string>("response.data.kernelAlias" + std::to_string(i));
            strncpy(cuRes->kernelAlias, kernelAlias.c_str(), XRM_MAX_NAME_LEN - 1);
            auto instanceName = rspTree.get<std::string>("response.data.instanceName" + std::to_string(i));
            strncpy(cuRes->instanceName, instanceName.c_str(), XRM_MAX_NAME_LEN - 1);
            auto cuName = rspTree.get<std::string>("response.data.cuName" + std::to_string(i));
            strncpy(cuRes->cuName, cuName.c_str(), XRM_MAX_NAME_LEN - 1);
            cuRes->deviceId = rspTree.get<int32_t>("response.data.deviceId" + std::to_string(i));
            cuRes->cuId = rspTree.get<int32_t>("response.data.cuId" + std::to_string(i));
            cuRes->channelId = rspTree.get<int32_t>("response.data.channelId" + std::to_string(i));
            auto cuType = rspTree.get<int32_t>("response.data.cuType" + std::to_string(i));
            cuRes->cuType = (xrmCuType)cuType;
            cuRes->allocServiceId = rspTree.get<uint64_t>("response.data.allocServiceId" + std::to_string(i));
            cuRes->channelLoad = rspTree.get<int32_t>("response.data.channelLoad" + std::to_string(i));
            cuRes->baseAddr = rspTree.get<uint64_t>("response.data.baseAddr" + std::to_string(i));
            cuRes->membankId = rspTree.get<uint32_t>("response.data.membankId" + std::to_string(i));
            cuRes->membankType = rspTree.get<uint32_t>("response.data.membankType" + std::to_string(i));
            cuRes->membankSize = rspTree.get<uint64_t>("response.data.membankSize" + std::to_string(i));
            cuRes->membankBaseAddr = rspTree.get<uint64_t>("response.data.membankBaseAddr" + std::to_string(i));
            cuRes->poolId = rspTree.get<int32_t>("response.data.poolId" + std::to_string(i));
        }
    }
    return (ret);
}

/**
 * \brief Blocking function of xrmCuAlloc(), this function will try to do cu allocation
 * until success.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuProp the property of cu.
 *             kernelName: the kernel name requested.
 *             kernelAlias: the alias of kernel name requested.
 *             devExcl: request exclusive device usage for this client.
 *             requestLoad: request load (1 - 100).
 *             poolId: request to allocate cu from specified resource pool.
 * @param interval the interval time (useconds) before re-trying, To set it as 0 to use XRM default interval
 * @param cuRes cu resource.
 *             xclbinFileName: xclbin (path and name) attached to this device.
 *             kernelPluginFileName: kernel plugin (only name) attached to this device.
 *             kernelName: the kernel name of allocated cu.
 *             kernelAlias: the name alias of allocated cu.
 *             instanceName: the instance name of allocated cu.
 *             cuName: the name of allocated cu (kernelName:instanceName).
 *             uuid: uuid of the loaded xclbin file.
 *             deviceId: device id of this cu.
 *             cuId: cu id of this cu.
 *             channelId: channel id of this cu.
 *             cuType: type of cu, hardware kernel or soft kernel.
 *             allocServiceId: service id for this cu allocation.
 *             channelLoad: allocated load of this cu (1 - 100).
 *             poolId: id of the cu pool this cu comes from, the default pool id is 0.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuBlockingAlloc(xrmContext context, xrmCuProperty* cuProp, uint64_t interval, xrmCuResource* cuRes) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuProp == NULL || cuRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu properties or resource pointer is NULL\n", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if ((cuProp->kernelName[0] == '\0') && (cuProp->kernelAlias[0] == '\0')) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s neither kernel name nor alias are provided", __func__);
        return (XRM_ERROR_INVALID);
    }
    if (cuProp->requestLoad <= 0 || cuProp->requestLoad > 100) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): wrong request load: %d", __func__, cuProp->requestLoad);
        return (XRM_ERROR_INVALID);
    }

    uint64_t useconds;
    if (interval == 0)
        useconds = XRM_DEFAULT_INTERVAL_US;
    else
        useconds = interval;
    if (!xrmIsCuExisting(ctx, cuProp))
        return (XRM_ERROR_NO_KERNEL);
    while (xrmCuAlloc(ctx, cuProp, cuRes) != XRM_SUCCESS) usleep(useconds);
    return (XRM_SUCCESS);
}

/**
 * \brief Blocking function of xrmCuListAlloc(), this function will try to do cu list allocation
 * until success.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuListProp the property of cu list.
 *             cuProps: cu prop list to fill kernelName, devExcl and requestLoad, starting from cuProps[0], no hole.
 *             cuNum: request number of cu in this list.
 *             sameDevice request this list of cu from same device.
 * @param interval the interval time (useconds) before re-trying, To set it as 0 to use XRM default interval
 * @param cuListRes cu list resource.
 *             cuResources: cu resource list to fill the allocated cus infor, starting from cuResources[0], no hole.
 *             cuNum: allocated cu number in this list.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuListBlockingAlloc(xrmContext context,
                               xrmCuListProperty* cuListProp,
                               uint64_t interval,
                               xrmCuListResource* cuListRes) {
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuListProp == NULL || cuListRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu list properties or resource pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuListProp->cuNum <= 0 || cuListProp->cuNum > XRM_MAX_LIST_CU_NUM) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): request list prop cuNum is %d, out of range from 1 to %d.\n",
               __func__, cuListProp->cuNum, XRM_MAX_LIST_CU_NUM);
        return (XRM_ERROR_INVALID);
    }

    uint64_t useconds;
    if (interval == 0)
        useconds = XRM_DEFAULT_INTERVAL_US;
    else
        useconds = interval;
    if (!xrmIsCuListExisting(ctx, cuListProp))
        return (XRM_ERROR_NO_KERNEL);
    while (xrmCuListAlloc(ctx, cuListProp, cuListRes) != XRM_SUCCESS) usleep(useconds);
    return (XRM_SUCCESS);
}

/**
 * \brief Blocking function of xrmCuGroupAlloc(), this function will try to do cu group
 * allocation until success.
 *
 * @param context the context created through xrmCreateContext()
 * @param cuGroupProp the property of cu group.
 *            udfCuGroupName: user defined cu group type name.
 *            poolId: id of the cu pool this group CUs come from, the system default pool id is 0.
 * @param interval the interval time (useconds) before re-trying, To set it as 0 to use XRM default interval
 * @param cuGroupRes cu group resource.
 *            cuResources cu resource group to fill the allocated cus infor, starting from cuResources[0], no hole.
 *            cuNum allocated cu number in this list.
 * @return int32_t, 0 on success or appropriate error number
 */
int32_t xrmCuGroupBlockingAlloc(xrmContext context,
                                xrmCuGroupProperty* cuGroupProp,
                                uint64_t interval,
                                xrmCuGroupResource* cuGroupRes) {
    int32_t ret = XRM_ERROR;
    int32_t i;
    xrmCuProperty* cuProp;
    xrmCuResource* cuRes;
    xrmPrivateContext* ctx = (xrmPrivateContext*)context;

    if (ctx == NULL || cuGroupProp == NULL || cuGroupRes == NULL) {
        xrmLog(XRM_LOG_ERROR, XRM_LOG_ERROR, "%s(): context, cu group property or resource pointer is NULL\n",
               __func__);
        return (XRM_ERROR_INVALID);
    }
    if (ctx->xrmApiVersion != XRM_API_VERSION_1) {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s wrong xrm api version %d", __func__, ctx->xrmApiVersion);
        return (XRM_ERROR_INVALID);
    }
    if (cuGroupProp->udfCuGroupName[0] == '\0') {
        xrmLog(ctx->xrmLogLevel, XRM_LOG_ERROR, "%s(): invalid input: udfCuGroupName is not provided.\n", __func__);
        return (XRM_ERROR_INVALID);
    }

    uint64_t useconds;
    if (interval == 0)
        useconds = XRM_DEFAULT_INTERVAL_US;
    else
        useconds = interval;
    if (!xrmIsCuGroupExisting(ctx, cuGroupProp))
        return (XRM_ERROR_NO_KERNEL);
    while (xrmCuGroupAlloc(ctx, cuGroupProp, cuGroupRes) != XRM_SUCCESS) usleep(useconds);
    return (XRM_SUCCESS);
}
