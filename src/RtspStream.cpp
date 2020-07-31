/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2016, Live Networks, Inc.  All rights reserved
// A demo application, showing how to create and run a RTSP client (that can potentially receive multiple streams concurrently).
//
// NOTE: This code - although it builds a running application - is intended only to illustrate how to develop your own RTSP
// client application.  For a full-featured RTSP client application - with much more functionality, and many options - see
// "openRTSP": http://www.live555.com/openRTSP/

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "wize/Log.h"
#include "RtspStream.h"


// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
#define REQUEST_STREAMING_OVER_TCP False



typedef boost::function<void(stream::CFrame const&)> StreamCallback;


// Forward function definitions:

// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void* clientData); // called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void* clientData);
void checkDisconnectHandler(void* clientData);
  // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// The main streaming routine (for each "rtsp://" URL):
void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, StreamCallback);

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

void usage(UsageEnvironment& env, char const* progName) {
  env << "Usage: " << progName << " <rtsp-url-1> ... <rtsp-url-N>\n";
  env << "\t(where each <rtsp-url-i> is a \"rtsp://\" URL)\n";
}

#if 0
char eventLoopWatchVariable = 0;

int test_main(int argc, char** argv) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // We need at least one "rtsp://" URL argument:
  if (argc < 2) {
    usage(*env, argv[0]);
    return 1;
  }

  // There are argc-1 URLs: argv[1] through argv[argc-1].  Open and start streaming each one:
  for (int i = 1; i <= argc-1; ++i) {
    openURL(*env, argv[0], argv[i]);
  }

  // All subsequent activity takes place within the event loop:
  env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
    // This function call does not return, unless, at some point in time, "eventLoopWatchVariable" gets set to something non-zero.

  return 0;

  // If you choose to continue the application past this point (i.e., if you comment out the "return 0;" statement above),
  // and if you don't intend to do anything more with the "TaskScheduler" and "UsageEnvironment" objects,
  // then you can also reclaim the (small) memory used by these objects by uncommenting the following code:
  /*
    env->reclaim(); env = NULL;
    delete scheduler; scheduler = NULL;
  */
}
#endif

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  MediaSubsessionIterator* iter;
  MediaSession* session;
  MediaSubsession* subsession;
  Boolean streamUsingTcp;
  TaskToken streamTimerTask;
  double duration;
  StreamCallback callback;
  int disconnectCounter;
  TaskToken checkDisconnectTask;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient: public RTSPClient {
public:
  static ourRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
          volatile char* eventLoopWatchVariable,
				  int verbosityLevel = 0,
				  char const* applicationName = NULL,
				  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment& env, char const* rtspURL, volatile char* eventLoopWatchVariable,
		int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum);
    // called only by createNew();
  virtual ~ourRTSPClient();

public:
  StreamClientState scs;
  volatile char* eventLoopWatchVariable;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.

class DummySink: public MediaSink {
public:
  static DummySink* createNew(UsageEnvironment& env,
			      MediaSubsession& subsession, // identifies the kind of data that's being received
                  char const* streamId, // identifies the stream itself (optional)
                  StreamCallback);

private:
  DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, StreamCallback);
    // called only by "createNew()"
  virtual ~DummySink();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
				struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
			 struct timeval presentationTime, unsigned durationInMicroseconds);

private:
  // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  StreamCallback mCallback;
  wize::CBuffer  mSPS;
  wize::CBuffer  mPPS;
  wize::CBuffer  mSEI;
  stream::CFrame mFrame;
  int            mSequence;
  u_int8_t* fReceiveBuffer;
  MediaSubsession& fSubsession;
  char* fStreamId;
};

#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

//static unsigned rtspClientCount = 0; // Counts how many streams (i.e., "RTSPClient"s) are currently in use.

void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, StreamCallback callback, volatile char* eventLoopWatchVariable) {
  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  ourRTSPClient* rtspClient = ourRTSPClient::createNew(env, rtspURL, eventLoopWatchVariable, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
  if (rtspClient == NULL) {
    env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
    return;
  }

  // set stream callback
  rtspClient->scs.callback = callback;

  //++rtspClientCount;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE);
}


// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
    env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL) {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!scs.session->hasSubsessions()) {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    unsigned uSecsToDelay = (unsigned)(1000000);
    scs.checkDisconnectTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)checkDisconnectHandler, rtspClient);

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

void setupNextSubsession(RTSPClient* rtspClient) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias
  
  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL) {
    if (!scs.subsession->initiate()) {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    } else {
      env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
      if (scs.subsession->rtcpIsMuxed()) {
	env << "client port " << scs.subsession->clientPortNum();
      } else {
	env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
      }
      env << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, scs.streamUsingTcp);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL) {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  } else {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  bool setupAgain = false;
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  do {
    if (resultCode != 0) {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      if (resultCode == 461 && !scs.streamUsingTcp) {
        // using tcp to setup again
        setupAgain = true;
        scs.streamUsingTcp = True;
      }
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
    if (scs.subsession->rtcpIsMuxed()) {
      env << "client port " << scs.subsession->clientPortNum();
    } else {
      env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
    }
    env << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    scs.subsession->sink = DummySink::createNew(env, *scs.subsession, rtspClient->url(), scs.callback);
      // perhaps use your own custom "MediaSink" subclass instead
    if (scs.subsession->sink == NULL) {
      env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
	  << "\" subsession: " << env.getResultMsg() << "\n";
      break;
    }

    env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
    scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
    scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
				       subsessionAfterPlaying, scs.subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (scs.subsession->rtcpInstance() != NULL) {
      scs.subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, scs.subsession);
    }
  } while (0);
  delete[] resultString;

  if (setupAgain) {
    // Continue setting up this subsession, by sending a RTSP "SETUP" command:
    rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, scs.streamUsingTcp);
  } else {
    // Set up the next subsession, if any:
    setupNextSubsession(rtspClient);
  }
}

void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean success = False;

  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0) {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0) {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
  }
}


// Implementation of the other event handlers:

void subsessionAfterPlaying(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  UsageEnvironment& env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\" on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  StreamClientState& scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void checkDisconnectHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = rtspClient->scs; // alias

  //env << *rtspClient << "check disconnect handler, counter:" << scs.disconnectCounter << "\n";
  if (++scs.disconnectCounter < 5) {
    // next round
    unsigned uSecsToDelay = (unsigned)(1000000);
    scs.checkDisconnectTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)checkDisconnectHandler, rtspClient);
    return;
  }

  env << *rtspClient << "shutdown stream, counter:" << scs.disconnectCounter << "\n";
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient* rtspClient, int exitCode) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession* subsession;

    while ((subsession = iter.next()) != NULL) {
      if (subsession->sink != NULL) {
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	if (subsession->rtcpInstance() != NULL) {
	  subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
	}

	someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
    // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

#if 0
  if (--rtspClientCount == 0) {
    // The final stream has ended, so exit the application now.
    // (Of course, if you're embedding this code into your own application, you might want to comment this out,
    // and replace it with "eventLoopWatchVariable = 1;", so that we leave the LIVE555 event loop, and continue running "main()".)
    exit(exitCode);
  }
#endif
  auto eventLoopWatchVariable = ((ourRTSPClient*)rtspClient)->eventLoopWatchVariable;
  *eventLoopWatchVariable = 1;
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL, volatile char* eventLoopWatchVariable,
					int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, eventLoopWatchVariable, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL, volatile char* eventLoopWatchVariable,
			     int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1)
  , eventLoopWatchVariable(eventLoopWatchVariable) {
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamUsingTcp(REQUEST_STREAMING_OVER_TCP)
  , streamTimerTask(NULL), duration(0.0), disconnectCounter(0), checkDisconnectTask(NULL) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    env.taskScheduler().unscheduleDelayedTask(checkDisconnectTask);
    Medium::close(session);
  }
}


// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 2*1024*1024

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, StreamCallback callback) {
  return new DummySink(env, subsession, streamId, callback);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, StreamCallback callback)
  : MediaSink(env),
    mCallback(callback),
    mSequence(0),
    fSubsession(subsession) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
}

DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// If you don't want to see debugging output for each received frame, then comment out the following line:
// #define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  // We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << this << " Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (uint32_t)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
#ifdef DEBUG_PRINT_NPT
  envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
  envir() << "\n";
#endif

  auto rtspClient = (ourRTSPClient*)fSubsession.miscPtr;
  rtspClient->scs.disconnectCounter = 0;  // reset counter

#if 0
    if (strcmp(fSubsession.mediumName(), "video") == 0)
    {
        static FILE* fp = fopen("e:/tmp/rtspclient0.h264", "wb");
        if (fp)
        {
#if 0
            if (mSequence == 0)
            {
                unsigned int num;
                SPropRecord *sps = parseSPropParameterSets(fSubsession.fmtp_spropparametersets(), num);
                // For H.264 video stream, we use a special sink that insert start_codes:
                struct timeval tv= {0,0};
                unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01};
                fwrite(start_code, 4, 1, fp);
                fwrite(sps[0].sPropBytes, sps[0].sPropLength, 1, fp);
                fwrite(start_code, 4, 1, fp);
                fwrite(sps[1].sPropBytes, sps[1].sPropLength, 1, fp);
                delete [] sps;
            }
#endif
            static char h264head[] = {0x00, 0x00, 0x00, 0x01};
            fwrite(h264head, 1, sizeof(h264head), fp);
            fwrite(fReceiveBuffer, 1, frameSize, fp);
        }
    }
#endif


  if (mCallback)
  {
    int channel = 0;
    int streamid = 0;
    uint64_t pts = (uint64_t)presentationTime.tv_sec * 1000 + (uint64_t)presentationTime.tv_usec / 1000;

    if (mFrame.empty())
    {
        // create frame
        if (strcmp(fSubsession.mediumName(), "video") == 0)
        {
            if (strcmp(fSubsession.codecName(), "JPEG") == 0)
            {
                // create jpeg image frame
                int w = fSubsession.videoWidth(), h = fSubsession.videoHeight();
                mFrame = stream::CFrameFactory::createImageFrame(
                            channel, streamid, w, h, pts, mSequence, stream::IMAGE_FORMAT_JPEG,
                            frameSize + numTruncatedBytes);

                uint8_t* ptr = (uint8_t*)mFrame.data();
                memcpy(ptr, fReceiveBuffer, frameSize);
            }
            else if (strcmp(fSubsession.codecName(), "H264") == 0)
            {
                static char nalHead[] = {0x00, 0x00, 0x00, 0x01};

                auto nalType = fReceiveBuffer[0] & 0x1f;
                if (nalType == stream::NALU_TYPE_SPS) {
                    // infof("sps comming! size(%d)\n", frameSize);
                    mSPS.resize(0);
                    mSPS.putBuffer(nalHead, sizeof(nalHead));
                    mSPS.putBuffer(fReceiveBuffer, frameSize);
                } else if (nalType == stream::NALU_TYPE_PPS) {
                    // infof("pps comming! size(%d)\n", frameSize);
                    mPPS.resize(0);
                    mPPS.putBuffer(nalHead, sizeof(nalHead));
                    mPPS.putBuffer(fReceiveBuffer, frameSize);
                } else if (nalType == stream::NALU_TYPE_SEI) {
                    // infof("sei comming! size(%d)\n", frameSize);
                    mSEI.resize(0);
                    mSEI.putBuffer(nalHead, sizeof(nalHead));
                    mSEI.putBuffer(fReceiveBuffer, frameSize);
                } else if (nalType == stream::NALU_TYPE_IDR) {
                    // create h264 video I frame
                    auto totalsize = mSPS.size() + mPPS.size() + mSEI.size() + sizeof(nalHead) + frameSize + numTruncatedBytes;
                    auto codec = stream::ENCODE_H264;
                    char frametype = 'I';
                    bool newformat = false;   // FIXME
                    // infof("create frame nal(%02x) type(%c) pts(%llu) len(%d)\n", nalType, frametype, pts, totalsize);
                    mFrame = stream::CFrameFactory::createVideoFrame(
                              channel, streamid, newformat, pts, mSequence,
                              codec, frametype, totalsize);

                    uint8_t* ptr = (uint8_t*)mFrame.data();
                    memcpy(ptr, mSPS.getBuffer(), mSPS.size());
                    ptr += mSPS.size();
                    mSPS.resize(0);
                    memcpy(ptr, mPPS.getBuffer(), mPPS.size());
                    ptr += mPPS.size();
                    mPPS.resize(0);
                    memcpy(ptr, mSEI.getBuffer(), mSEI.size());
                    ptr += mSEI.size();
                    mSEI.resize(0);
                    memcpy(ptr, nalHead, sizeof(nalHead));
                    ptr += sizeof(nalHead);
                    memcpy(ptr, fReceiveBuffer, frameSize);

                    ++mSequence;
                } else if (nalType == stream::NALU_TYPE_SLICE) {
                    // create h264 video P frame
                    auto totalsize = sizeof(nalHead) + frameSize + numTruncatedBytes;
                    auto codec = stream::ENCODE_H264;
                    char frametype = 'P';
                    bool newformat = false;
                    // infof("create frame nal(%02x) type(%c) pts(%llu) len(%d)\n", nalType, frametype, pts, totalsize);
                    mFrame = stream::CFrameFactory::createVideoFrame(
                              channel, streamid, newformat, pts, mSequence,
                              codec, frametype, totalsize);

                    uint8_t* ptr = (uint8_t*)mFrame.data();
                    memcpy(ptr, nalHead, sizeof(nalHead));
                    ptr += sizeof(nalHead);
                    memcpy(ptr, fReceiveBuffer, frameSize);

                    ++mSequence;
                } else {
                    infof("ignored nal(%02x) bytes(%d)\n", nalType, frameSize);
                }
            }
        }
        else if (strcmp(fSubsession.mediumName(), "audio") == 0)
        {
            // TODO: create audio frame
        }
    }
    else
    {
        // push remain data to frame
        int pos = mFrame.size() - (frameSize + numTruncatedBytes);
        if (pos >= 0 && pos + (int)frameSize < (int)mFrame.size())
        {
            uint8_t* ptr = (uint8_t*)mFrame.data() + pos;
            memcpy(ptr, fReceiveBuffer, frameSize);
        }
        else
        {
            warnf("pos out of range! pos(%d) frameSize(%d) numTruncatedBytes(%d) FrameSize(%d)\n",
                  pos, frameSize, numTruncatedBytes, mFrame.size());
        }
    }

    if (!mFrame.empty() && numTruncatedBytes == 0)
    {
        mCallback(mFrame);
        mFrame = stream::CFrame();
    }
  }

  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}



////////////////////////////////////////////////////////////////////////////////


namespace live555client {


CRtspStreamSource::CRtspStreamSource(const char* uri)
    : wize::CLoopThread("RtspClient")
    , mUri(uri ? uri : "")
    , mEventLoopWatchVariable(0)
{
    tracepoint();
}

CRtspStreamSource::~CRtspStreamSource()
{
    tracepoint();
    stop();
}

CRtspStreamSource::Connection CRtspStreamSource::connect(StreamCallback callback)
{
    return mSignal.connect(callback);
}

/// 开启
bool CRtspStreamSource::start()
{
    tracepoint();
    return startThread();
}

/// 停止
bool CRtspStreamSource::stop()
{
    tracepoint();
    mEventLoopWatchVariable = 1;
    return stopThread();
}

void CRtspStreamSource::threadProc()
{
    tracef("__begin!\n");
    int sleepms = 1000;

    while (true) {
      mEventLoopWatchVariable = 0;

      // Begin by setting up our usage environment:
      TaskScheduler* scheduler = BasicTaskScheduler::createNew();
      UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

      // Open and start streaming
      openURL(*env, "RtspClient", mUri.c_str(), boost::bind(&CRtspStreamSource::onStreamCallback, this, _1), &mEventLoopWatchVariable);

      // All subsequent activity takes place within the event loop:
      env->taskScheduler().doEventLoop(&mEventLoopWatchVariable);
      // This function call does not return, unless, at some point in time, "mEventLoopWatchVariable" gets set to something non-zero.

      env->reclaim(); env = NULL;
      delete scheduler; scheduler = NULL;

      sleepms = (sleepms >= 8000) ? 2000 : sleepms * 2;
      tracef("wait (%d)ms to retry open rtsp client...\n", sleepms);
      auto sig = waitSignal(sleepms);
      if (sig == SIGNAL_EXIT) {
        tracef("exit by user stop!\n");
        break;
      }
    }

    tracef("__end!\n");
}

void CRtspStreamSource::onStreamCallback(stream::CFrame const& frame)
{
    // tracepoint();
#if 0
    auto info = frame.info();
    if (info->type == stream::STREAM_VIDEO)
    {
        static FILE* fp = fopen("e:/tmp/rtspclient.h264", "w");
        if (fp)
        {
            fwrite(frame.data(), 1, frame.size(), fp);
        }
    }
#endif
    mSignal(frame);
}


} // namespace rtsp

