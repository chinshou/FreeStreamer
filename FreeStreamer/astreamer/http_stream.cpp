/*
 * This file is part of the FreeStreamer project,
 * (C)Copyright 2011 Matias Muhonen <mmu@iki.fi>
 * See the file ''LICENSE'' for using the code.
 */

#include "http_stream.h"
#include "audio_queue.h"

namespace astreamer {

const size_t HTTP_Stream::STREAM_BUFSIZ = Audio_Queue::AQ_BUFSIZ;

CFStringRef HTTP_Stream::httpRequestMethod   = CFSTR("GET");
CFStringRef HTTP_Stream::httpUserAgentHeader = CFSTR("User-Agent");
CFStringRef HTTP_Stream::httpUserAgentValue  = CFSTR("aStreamer/1.0");
CFStringRef HTTP_Stream::icyMetaDataHeader = CFSTR("Icy-MetaData");
CFStringRef HTTP_Stream::icyMetaDataValue  = CFSTR("1"); /* always request ICY metadata, if available */

    
/* HTTP_Stream: public */
HTTP_Stream::HTTP_Stream(CFURLRef url, HTTP_Stream_Delegate *delegate) :
    m_readStream(0),
    m_delegate(delegate),
    m_url((CFURLRef)CFRetain(url)),
    m_httpHeadersParsed(false),
    
    m_icyStream(false),
    m_icyHeaderCR(false),
    m_icyHeadersRead(false),
    m_icyHeadersParsed(false),
    
    m_icyMetaDataInterval(0),
    m_dataByteReadCount(0),
    m_metaDataBytesRemaining(0),
    
    m_httpReadBuffer(0),
    m_icyReadBuffer(0)
{
}

HTTP_Stream::~HTTP_Stream()
{
    close();
    
    if (m_httpReadBuffer) {
        delete m_httpReadBuffer, m_httpReadBuffer = 0;
    }
    if (m_icyReadBuffer) {
        delete m_icyReadBuffer, m_icyReadBuffer = 0;
    }
    
    CFRelease(m_url), m_url = 0;
}
    
std::string HTTP_Stream::contentType()
{
    return m_contentType;
}

bool HTTP_Stream::open()
{
    bool success = false;
    CFStreamClientContext CTX = { 0, this, NULL, NULL, NULL };
    
    /* Already opened a read stream, return */
    if (m_readStream) {
        goto out;
    }
    
    /* Reset state */
    m_httpHeadersParsed = false;
    m_contentType = "";
    
    m_icyStream = false;
    m_icyHeaderCR = false;
    m_icyHeadersRead = false;
    m_icyHeadersParsed = false;
    
    m_icyHeaderLines.clear();
    m_icyMetaDataInterval = 0;
    m_dataByteReadCount = 0;
    m_metaDataBytesRemaining = 0;
    m_icyMetaData = "";
	
    /* Failed to create a stream */
    if (!(m_readStream = createReadStream(m_url))) {
        goto out;
    }
    
    if (!CFReadStreamSetClient(m_readStream, kCFStreamEventHasBytesAvailable |
	                                         kCFStreamEventEndEncountered |
	                                         kCFStreamEventErrorOccurred, readCallBack, &CTX)) {
        CFRelease(m_readStream), m_readStream = 0;
        goto out;
    }
    
    /* Add to the run loop */
    CFReadStreamScheduleWithRunLoop(m_readStream, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    
    if (!CFReadStreamOpen(m_readStream)) {
        /* Open failed: clean */
        CFReadStreamSetClient(m_readStream, 0, NULL, NULL);
        CFReadStreamUnscheduleFromRunLoop(m_readStream, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
        CFRelease(m_readStream), m_readStream = 0;
        goto out;
    }
    
    success = true;

out:
    return success;
}

void HTTP_Stream::close()
{
    if (m_readStream) {
        CFReadStreamSetClient(m_readStream, 0, NULL, NULL);
        CFReadStreamUnscheduleFromRunLoop(m_readStream, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
        CFReadStreamClose(m_readStream);
        CFRelease(m_readStream), m_readStream = 0;
    }
}
    
/* private */
    
CFReadStreamRef HTTP_Stream::createReadStream(CFURLRef url)
{
    CFReadStreamRef readStream = 0;
    CFHTTPMessageRef request;
    CFDictionaryRef proxySettings;
    
    if (!(request = CFHTTPMessageCreateRequest(kCFAllocatorDefault, httpRequestMethod, url, kCFHTTPVersion1_1))) {
        goto out;
    }
    
    CFHTTPMessageSetHeaderFieldValue(request, httpUserAgentHeader, httpUserAgentValue);
    CFHTTPMessageSetHeaderFieldValue(request, icyMetaDataHeader, icyMetaDataValue);
    
    if (!(readStream = CFReadStreamCreateForStreamedHTTPRequest(kCFAllocatorDefault, request, 0))) {
        goto out;
    }
    
    CFReadStreamSetProperty(readStream,
                            kCFStreamPropertyHTTPShouldAutoredirect,
                            kCFBooleanTrue);
    
    proxySettings = CFNetworkCopySystemProxySettings();
    CFReadStreamSetProperty(readStream, kCFStreamPropertyHTTPProxy, proxySettings);
    CFRelease(proxySettings);
    
out:
    if (request) {
        CFRelease(request);
    }
    
    return readStream;
}
    
void HTTP_Stream::parseHttpHeadersIfNeeded(UInt8 *buf, CFIndex bufSize)
{
    if (m_httpHeadersParsed) {
        return;
    }
    m_httpHeadersParsed = true;
    
    /* If the response has the "ICY 200 OK" string,
     * we are dealing with the ShoutCast protocol.
     * The HTTP headers won't be available.
     */
    std::string header;
    
    for (CFIndex k=0; k < bufSize; k++) {
        UInt8 c = buf[k];
        // Ignore non-ASCII chars
        if (c < 32 || c > 126) {
            continue;
        }
        header.push_back(c);
    }
    
    char *p = strstr(header.c_str(), "ICY 200 OK");
    
    // This is an ICY stream, don't try to parse the HTTP headers
    if (p) {
        m_icyStream = true;
        return;
    }
    
    CFHTTPMessageRef response = (CFHTTPMessageRef)CFReadStreamCopyProperty(m_readStream, kCFStreamPropertyHTTPResponseHeader);
    if (response) {
        /*
         * If the server responded with the icy-metaint header, the response
         * body will be encoded in the ShoutCast protocol.
         */
        CFStringRef icyMetaIntString = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("icy-metaint"));
        if (icyMetaIntString) {
            m_icyStream = true;
            m_icyHeadersParsed = true;
            m_icyHeadersRead = true;
            m_icyMetaDataInterval = CFStringGetIntValue(icyMetaIntString);
            CFRelease(icyMetaIntString);
        }
        
        CFStringRef contentTypeString = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Content-Type"));
        if (contentTypeString) {
            char s[256] = {0};
            CFStringGetCString(contentTypeString, s, sizeof s, kCFStringEncodingUTF8);
        
            if (s) {
                m_contentType.append(s);
            }
        
            CFRelease(contentTypeString);
        }
        CFRelease(response);
    }
       
    if (m_delegate) {
        m_delegate->streamIsReadyRead();
    }
}
    
void HTTP_Stream::parseICYStream(UInt8 *buf, CFIndex bufSize)
{
    CFIndex offset = 0;
    if (!m_icyHeadersRead) {
        // TODO: fail after n tries?
        
        for (; offset < bufSize; offset++) {
            if (m_icyHeaderCR && buf[offset] == '\n') {
                m_icyHeaderLines.push_back(std::string(""));
                
                size_t n = m_icyHeaderLines.size();
                
                /* If the last two lines were empty, we have reached
                 the end of the headers */
                if (n >= 2) {                    
                    if (m_icyHeaderLines[n-2].empty() &&
                        m_icyHeaderLines[n-1].empty()) {
                        m_icyHeadersRead = true;
                        break;
                    }
                }
                
                continue;
            }
            
            if (buf[offset] == '\r') {
                m_icyHeaderCR = true;
                continue;
            } else {
                m_icyHeaderCR = false;
            }
            
            size_t numberOfLines = m_icyHeaderLines.size();
            
            if (numberOfLines == 0) {
                continue;
            }
            m_icyHeaderLines[numberOfLines - 1].push_back(buf[offset]);
        }
    } else if (!m_icyHeadersParsed) {
        const char *icyContentTypeHeader = "content-type:";
        const char *icyMetaDataHeader = "icy-metaint:";
        size_t icyContenTypeHeaderLength = strlen(icyContentTypeHeader);
        size_t icyMetaDataHeaderLength = strlen(icyMetaDataHeader);
        
        for (std::vector<std::string>::iterator h = m_icyHeaderLines.begin(); h != m_icyHeaderLines.end(); ++h) {
            if (h->compare(0, icyContenTypeHeaderLength, icyContentTypeHeader) == 0) {
                m_contentType = h->substr(icyContenTypeHeaderLength, h->length() - icyContenTypeHeaderLength);
            }
            
            if (h->compare(0, icyMetaDataHeaderLength, icyMetaDataHeader) == 0) {
                m_icyMetaDataInterval = atoi(h->substr(icyMetaDataHeaderLength, h->length() - icyMetaDataHeaderLength).c_str());
            }
        }
        
        m_icyHeadersParsed = true;
        offset++;
        
        if (m_delegate) {
            m_delegate->streamIsReadyRead();
        }
    }
    
    if (!m_icyReadBuffer) {
        m_icyReadBuffer = new UInt8[STREAM_BUFSIZ];
    }
    
    size_t i=0;
    
    for (; offset < bufSize; offset++) {
        // is this a metadata byte?
        if (m_metaDataBytesRemaining > 0) {
            m_metaDataBytesRemaining--;
            
            if (m_metaDataBytesRemaining == 0) {
                m_dataByteReadCount = 0;
                
                if (m_delegate && !m_icyMetaData.empty()) {
                    m_delegate->streamMetaDataAvailable(m_icyMetaData);
                }
                m_icyMetaData.clear();
                continue;
            }
            
            char c = buf[offset];
            
            if (c < 32 || c > 126) {
                continue;
            }
            
            m_icyMetaData.push_back(c);
            continue;
        }
        
        // is this the interval byte?
        if (m_icyMetaDataInterval > 0 && m_dataByteReadCount == m_icyMetaDataInterval) {
            m_metaDataBytesRemaining = buf[offset] * 16;
            
            if (m_metaDataBytesRemaining == 0) {
                m_dataByteReadCount = 0;
            }
            continue;
        }
        
        // a data byte
        m_dataByteReadCount++;
        m_icyReadBuffer[i++] = buf[offset];
    }
    
    if (m_delegate && i > 0) {
        m_delegate->streamHasBytesAvailable(m_icyReadBuffer, i);
    }
}
    
void HTTP_Stream::readCallBack(CFReadStreamRef stream, CFStreamEventType eventType, void *clientCallBackInfo)
{
    HTTP_Stream *THIS = static_cast<HTTP_Stream*>(clientCallBackInfo);
    
    if (!THIS->m_delegate) {
        return;
    }
    
    switch (eventType) {
        case kCFStreamEventHasBytesAvailable: {
            if (!THIS->m_httpReadBuffer) {
                THIS->m_httpReadBuffer = new UInt8[STREAM_BUFSIZ];
            }
            
            CFIndex bytesRead = CFReadStreamRead(stream, THIS->m_httpReadBuffer, STREAM_BUFSIZ);
            
            if (bytesRead < 0) {
                THIS->m_delegate->streamErrorOccurred();
                break;
            }
            
            if (bytesRead > 0) {
                THIS->parseHttpHeadersIfNeeded(THIS->m_httpReadBuffer, bytesRead);
                
                if (THIS->m_icyStream) {
                    THIS->parseICYStream(THIS->m_httpReadBuffer, bytesRead);
                } else {
                    THIS->m_delegate->streamHasBytesAvailable(THIS->m_httpReadBuffer, bytesRead);
                }
            } 
            break;
        }
        case kCFStreamEventEndEncountered: {
            THIS->m_delegate->streamEndEncountered();
            break;
        }
        case kCFStreamEventErrorOccurred: {
            THIS->m_delegate->streamErrorOccurred();
            break;
        }
    }
}

}  // namespace astreamer
