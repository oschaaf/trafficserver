// Copyright 2013 We-Amp B.V.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: oschaaf@we-amp.com (Otto van der Schaaf)
#include <ts/ts.h>

#include <stdio.h>

#include "ats_resource_intercept.h"


#include "ats_base_fetch.h"
#include "ats_rewrite_driver_factory.h"
#include "ats_rewrite_options.h"
#include "ats_server_context.h"
#include "ats_speed.h"

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/system/public/system_request_context.h"

#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/system/public/handlers.h"

using namespace net_instaweb;

struct InterceptCtx
{
  TSVConn vconn;
  TSIOBuffer req_buffer;
  TSIOBufferReader req_reader;
  TSIOBuffer resp_buffer;
  TSIOBufferReader resp_reader;
  GoogleString* response;
  TransformCtx* request_ctx;
  RequestHeaders* request_headers;
  
  InterceptCtx()
      : vconn(NULL)
      , req_buffer(NULL)
      , req_reader(NULL)
      , resp_buffer(NULL)
      , resp_reader(NULL)
      , response( new GoogleString() )
      , request_ctx(NULL)
      , request_headers(NULL)
  {
  };
};

static void
shutdown (TSCont cont, InterceptCtx * intercept_ctx) {
  if (intercept_ctx->req_reader != NULL) {
    TSIOBufferReaderFree(intercept_ctx->req_reader);
    intercept_ctx->req_reader = NULL;
  }
  if (intercept_ctx->req_buffer != NULL) {
    TSIOBufferDestroy(intercept_ctx->req_buffer);
    intercept_ctx->req_buffer = NULL;
  }
  if (intercept_ctx->resp_reader != NULL) {
    TSIOBufferReaderFree(intercept_ctx->resp_reader);
    intercept_ctx->resp_reader = NULL;
  }
  if (intercept_ctx->resp_buffer != NULL) {
    TSIOBufferDestroy(intercept_ctx->resp_buffer);
    intercept_ctx->resp_buffer = NULL;
  }
  if (intercept_ctx->vconn != NULL) {
    TSVConnShutdown(intercept_ctx->vconn, 0, 1);
    TSVConnClose(intercept_ctx->vconn);
    intercept_ctx->vconn = NULL;
  }
  if (intercept_ctx->response != NULL) {
    delete intercept_ctx->response;
    intercept_ctx->response = NULL;
  }
  // TODO(oschaaf): think the ordering of this one through.
  if (intercept_ctx->request_ctx) {
    ats_ctx_destroy(intercept_ctx->request_ctx);
    intercept_ctx->request_ctx = NULL;
  }
  if (intercept_ctx->request_headers != NULL) {
    delete intercept_ctx->request_headers;
    intercept_ctx->request_headers = NULL;
  }
  delete intercept_ctx;
  TSContDestroy(cont);
}

static int
resource_intercept(TSCont cont, TSEvent event, void *edata)
{
  InterceptCtx *intercept_ctx = static_cast<InterceptCtx *>(TSContDataGet(cont));
  bool shutDown = false;

  // TODO(oschaaf): have a look at https://github.com/apache/trafficserver/blob/master/plugins/experimental/esi/serverIntercept.c
  // and see if we have any edge cases we should fix.
  switch (event) {
    case TS_EVENT_NET_ACCEPT: {
      intercept_ctx->vconn = static_cast<TSVConn>(edata);
      intercept_ctx->req_buffer = TSIOBufferCreate();
      intercept_ctx->req_reader = TSIOBufferReaderAlloc(intercept_ctx->req_buffer);
      intercept_ctx->resp_buffer = TSIOBufferCreate();
      intercept_ctx->resp_reader = TSIOBufferReaderAlloc(intercept_ctx->resp_buffer);
      TSVConnRead(intercept_ctx->vconn, cont, intercept_ctx->req_buffer, 0x7fffffff);
    } break;
    case TS_EVENT_VCONN_READ_READY: {
      CHECK(intercept_ctx->request_ctx->base_fetch == NULL) << "Base fetch must not be set!";
      CHECK(intercept_ctx->request_ctx->url_string != NULL) << "Url must be set!";

      TSVConnShutdown(intercept_ctx->vconn, 1, 0);

      // response will already have a size for internal pages at this point.
      // resources, however, will have to be fetched.
      // TODO(oschaaf): this is extremely ugly.
      if (intercept_ctx->response->size() == 0) { 
        // TODO(oschaaf): unused - must we close / clean this up?
        TSVIO downstream_vio = TSVConnWrite(
            intercept_ctx->vconn, cont, intercept_ctx->resp_reader, 0x7fffffff);

        AtsServerContext* server_context = intercept_ctx->request_ctx->server_context;

        // TODO:(oschaaf) host/port
        SystemRequestContext* system_request_context = 
            new SystemRequestContext(server_context->thread_system()->NewMutex(),
                                     server_context->timer(),
                                     80,
                                     "127.0.0.1");

        intercept_ctx->request_ctx->base_fetch = new AtsBaseFetch(
            server_context, RequestContextPtr(system_request_context),
            downstream_vio, intercept_ctx->resp_buffer, true);
        intercept_ctx->request_ctx->base_fetch->set_request_headers(
            intercept_ctx->request_headers);

        RewriteOptions* options = NULL;
         
        //const char* host = intercept_ctx->request_headers->Lookup1(HttpAttributes::kHost);
        const char* host = intercept_ctx->request_ctx->gurl->HostAndPort().as_string().c_str();
        if (host != NULL && strlen(host) > 0) {
          intercept_ctx->request_ctx->options = get_host_options(host);
        }
        
        // TODO(oschaaf): directory options should be coming from configuration!
        bool ok = ps_determine_options(server_context,
                                       intercept_ctx->request_ctx->options,
                                       intercept_ctx->request_ctx->base_fetch->request_headers(),
                                       intercept_ctx->request_ctx->base_fetch->response_headers(),
                                       &options,
                                       intercept_ctx->request_ctx->gurl);
        
        // Take ownership of custom_options.
        scoped_ptr<RewriteOptions> custom_options(options);
        
        if (!ok) {
          TSError("Failure while determining request options for psol resource");
          // options = server_context->global_options();
        } else {
          // ps_determine_options modified url, removing any ModPagespeedFoo=Bar query
          // parameters.  Keep url_string in sync with url.
          // TODO(oschaaf): we really should determine if we have to do the lookup
          intercept_ctx->request_ctx->gurl->Spec().CopyToString(intercept_ctx->request_ctx->url_string);
        }

        // The url we have here is already checked for IsWebValid()
        net_instaweb::ResourceFetch::Start(
            GoogleUrl(*intercept_ctx->request_ctx->url_string),
            custom_options.release() /* null if there aren't custom options */,
            false /* using_spdy */, server_context, intercept_ctx->request_ctx->base_fetch);
      } else {
        int64_t numBytesToWrite, numBytesWritten;
        numBytesToWrite = intercept_ctx->response->size();
        numBytesWritten = TSIOBufferWrite(intercept_ctx->resp_buffer,
                                          intercept_ctx->response->c_str(), numBytesToWrite);
        
        if (numBytesWritten == numBytesToWrite) {
          TSVConnWrite(intercept_ctx->vconn, cont, intercept_ctx->resp_reader, numBytesToWrite);
        } else {
          TSError("Not all output could be written in one go");
          DCHECK(false);
        }
      }
    }  break;
    case TS_EVENT_VCONN_EOS:
      TSVConnShutdown(intercept_ctx->vconn, 1, 0);
      break;
    case TS_EVENT_VCONN_READ_COMPLETE: {
      TSVConnShutdown(intercept_ctx->vconn, 1, 0);
    } break;
    case TS_EVENT_VCONN_WRITE_READY:
      break;
    case TS_EVENT_VCONN_WRITE_COMPLETE:
      shutDown = true;
      break;
    case TS_EVENT_ERROR:
      TSError("vconn event: error %s", intercept_ctx->request_ctx->url_string->c_str());
      shutDown = true;
      break;
    case TS_EVENT_NET_ACCEPT_FAILED:
      TSError("vconn event: accept failed");
      shutDown = true;
      break;
    case TS_EVENT_IMMEDIATE:
    case TS_EVENT_TIMEOUT:
      break;
    default:
      TSError("default clause event: %d", event);
      break;
  }

  if (shutDown) {
    shutdown(cont, intercept_ctx);
  }

  return 1;
}

// We intercept here because serving from ats's own cache is faster
// then serving from pagespeed's cache. (which needs to be looked in to)
static int
read_cache_header_callback(TSCont cont, TSEvent event, void *edata)
{
  TSHttpTxn txn = static_cast<TSHttpTxn>(edata);
  TransformCtx* ctx = get_transaction_context(txn);

  if (ctx == NULL) {
    TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
    return 0;
  }
  if (!ctx->resource_request) {
    TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
    return 0;    
  }
  // TODO(oschaaf): FIXME: Ownership of ctx has become too mucky.
  // This is because I realised too late that the intercepts
  // are able to outlive the transaction, which I hacked
  // to work.
  if (TSHttpIsInternalRequest(txn) == TS_SUCCESS) {
    ats_ctx_destroy(ctx);
    TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
    return 0;
  }
  
  if (cache_hit(txn)) {
    ats_ctx_destroy(ctx);
    TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
    return 0;
  }

  AtsServerContext* server_context = ctx->server_context;
  AtsRewriteDriverFactory* factory = (AtsRewriteDriverFactory*)server_context->factory();
  GoogleString output;
  StringWriter writer(&output);
  HttpStatus::Code status = HttpStatus::kOK;
  ContentType content_type = kContentTypeHtml;
  StringPiece cache_control = HttpAttributes::kNoCache;
  const char* error_message = NULL;
  StringPiece request_uri_path = ctx->gurl->PathAndLeaf();  
  
  if (ctx->gurl->PathSansQuery() == "/robots.txt") {
    content_type = kContentTypeText;
    writer.Write("User-agent: *\n", server_context->message_handler());
    writer.Write("Disallow: /\n", server_context->message_handler());
  } else if (ctx->gurl->PathSansQuery() == "/pagespeed_message") {    
    // TODO(oschaaf)... let's wait for a bit with this one.
  } else if (ctx->gurl->PathSansQuery() == "/pagespeed_statistics" || ctx->gurl->PathSansQuery() == "/pagespeed_global_statistics") {
    error_message = StatisticsHandler(
        factory,
        server_context,
        NULL,  // No SPDY-specific config in ats_pagespeed.
        !factory->use_per_vhost_statistics() || StringCaseStartsWith(
            request_uri_path, "/pagespeed_global_statistics"),
        StringPiece(ctx->gurl->Query().as_string().c_str()),
        &content_type,
        &writer,
        server_context->message_handler());
  } else if (ctx->gurl->PathSansLeaf() == "/ats_speed_static/") {
    StringPiece file_contents;
    if (server_context->static_asset_manager()->GetAsset(
            request_uri_path.substr(
                strlen("/ats_speed_static/")),
            &file_contents, &content_type, &cache_control)) {
      file_contents.CopyToString(&output);
    } else {
      error_message = "Static asset not found";
    }    
  }  else if (ctx->gurl->PathSansQuery() == "/pagespeed_console") {
    ConsoleHandler(server_context, server_context->config(), &writer, server_context->message_handler());
  } else { 
    // Optimized resource are highly cacheable (1 year expiry)
    // TODO(oschaaf): configuration
    TSHttpTxnRespCacheableSet(txn, 1);
    TSHttpTxnReqCacheableSet(txn, 1);

    TSMBuffer reqp;
    TSMLoc req_hdr_loc;
    if (TSHttpTxnClientReqGet(ctx->txn, &reqp, &req_hdr_loc) != TS_SUCCESS) {
      TSError("Error TSHttpTxnClientReqGet for resource!");
      TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
      return 0;
    }
    
    TSCont interceptCont = TSContCreate(resource_intercept, TSMutexCreate());
    InterceptCtx *intercept_ctx = new InterceptCtx();
    intercept_ctx->request_ctx = ctx;
    intercept_ctx->request_headers = new RequestHeaders();
    copy_request_headers_to_psol(reqp, req_hdr_loc, intercept_ctx->request_headers);
    TSHandleMLocRelease(reqp, TS_NULL_MLOC, req_hdr_loc);

    
    TSContDataSet(interceptCont, intercept_ctx);
    TSHttpTxnServerIntercept(interceptCont, txn);
    TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
    return 0;
  } 

  if (error_message != NULL) {
    status = HttpStatus::kNotFound;
    content_type = kContentTypeHtml;
    output = error_message;
  }

  ResponseHeaders response_headers;
  response_headers.SetStatusAndReason(status);
  response_headers.set_major_version(1);
  response_headers.set_minor_version(0);

  response_headers.Add(HttpAttributes::kContentType, content_type.mime_type());

  int64 now_ms = factory->timer()->NowMs();
  response_headers.SetDate(now_ms);
  response_headers.SetLastModified(now_ms);
  response_headers.Add(HttpAttributes::kCacheControl, cache_control);

  if (FindIgnoreCase(cache_control, "private") ==
      static_cast<int>(StringPiece::npos)) {
    response_headers.Add(HttpAttributes::kEtag, "W/\"0\"");
  }

  GoogleString header;
  StringWriter header_writer(&header);
  response_headers.WriteAsHttp(&header_writer, server_context->message_handler());
  
  TSCont interceptCont = TSContCreate(resource_intercept, TSMutexCreate());
  InterceptCtx *intercept_ctx = new InterceptCtx(); 
  intercept_ctx->request_ctx = ctx;
  header.append(output);
  TSHttpTxnRespCacheableSet(txn, 0);
  TSHttpTxnReqCacheableSet(txn, 0);
  TSContDataSet(interceptCont, intercept_ctx);
  TSHttpTxnServerIntercept(interceptCont, txn);
  intercept_ctx->response->append(header);
  
  TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

void setup_resource_intercept()
{
  TSCont cont = TSContCreate(read_cache_header_callback, NULL);
  TSHttpHookAdd(TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, cont);
}

