#include <uv.h>
#include <stdio.h>
#include <string>
#include <windows.h>
#include <vector>
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#include <node.h>
#include <nan.h>

#if WINDOWS 
#define ASIO_DRIVER_NAME "ASIO4ALL v2"
#elif MAC
#define ASIO_DRIVER_NAME "ASIO4ALL v2"
#endif
#define MAX_THREADS 3
#define BUF_SIZE 255

using namespace v8;

enum {
	kMaxInputChannels = 32,
	kMaxOutputChannels = 32
};

Persistent<Array> buffersForInput;

typedef struct DriverInfo{
	//A strucutre needed by ASIOInit do initialize the AudioSteamIO
	//it holds various information about the driver
	ASIODriverInfo driverInfo;

	//ASIOGetChannels() number of availible audio channels
	long inputChannels;
	long outputChannels;

	//ASIOGetBufferSize() get the supported audio buffer sizes 
	long minSize;
	long maxSize;
	long preferredSize;
	long granularity;

	//ASIOGetSampleRate() get the current sample rate
	ASIOSampleRate sampleRate;

	// ASIOOutputReady() true-if supported by the card
	bool postOutput;

	//ASIOGetLatencies() query for the constant audio latencies relative to the bufferSwitch() callback
	long           inputLatency;
	long           outputLatency;

	//Allocate memory for the audio buffers and allocate hardware resources for the audio channels. ASIOCreateBuffers() 
	long inputBuffers;	// becomes number of actual created input buffers
	long outputBuffers;	// becomes number of actual created output buffers
	//holds various information about buffer, Input/ouput, ChannelNum, buffer address
	ASIOBufferInfo bufferInfos[kMaxInputChannels + kMaxOutputChannels];
	// ASIOGetChannelInfo() get information about a specific channel (sample type, name, word clock group)
	ASIOChannelInfo channelInfos[kMaxInputChannels + kMaxOutputChannels];
	// The above two arrays share the same indexing, as the data in them are linked together

	// Information from ASIOGetSamplePosition() Inquires the sample position/time stamp pair. 
	// data is converted to double floats for easier use, however 64 bit integer can be used, too
	double nanoSeconds;
	double samples;
	double tcSamples;	// time code samples

	// bufferSwitchTimeInfo()
	ASIOTime tInfo;			// time info state
	unsigned long sysRefTime;      // system reference time, when bufferSwitch() was called

	Persistent<Function> callback;
	Isolate * isolate;
	
}DriverInfo;
typedef struct Work{
	uv_work_t request;
	int index;
}Work;

DriverInfo asioDriverInfo = { 0 };
//struct hold all the asioCallbacks function addresses ?
ASIOCallbacks asioCallbacks;
// some external references
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char *name);

// internal prototypes (required for the Metrowerks CodeWarrior compiler)
int main(int argc, char* argv[]);
long initAsioDriverInfo(DriverInfo *asioDriverInfo);
ASIOError createAsioBuffers(DriverInfo *asioDriverInfo, std::string end, std::vector<int> iC, std::vector<int> oC);
unsigned long getSysReferenceTime();
static void WorkAsync(uv_work_t *req);
static void WorkAsyncComplete(uv_work_t *req,int status);

// callback prototypes
void bufferSwitch(long index, ASIOBool processNow);
ASIOTime *bufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool processNow);
void sampleRateChanged(ASIOSampleRate sRate);
long asioMessages(long selector, long value, void* message, double* opt);

long initAsioDriverInfo(DriverInfo *asioDriverInfo, int sR, int bps, int spb, std::string end, std::vector<int> iC, std::vector<int> oC){	
	// collect the informational data of the driver
	// get the number of available channels
	
	if (ASIOGetChannels(&asioDriverInfo->inputChannels, &asioDriverInfo->outputChannels) == ASE_OK)
	{
		printf("Availible audio channels (inputs: %d, outputs: %d);\n", asioDriverInfo->inputChannels, asioDriverInfo->outputChannels);
		printf("Used audio channels (inputs: %d, outputs: %d);\n", iC.size(), oC.size());
		asioDriverInfo->outputChannels = (long)oC.size();
		asioDriverInfo->inputChannels = (long)iC.size();
		// get the usable buffer sizes
		if (ASIOGetBufferSize(&asioDriverInfo->minSize, &asioDriverInfo->maxSize, &asioDriverInfo->preferredSize, &asioDriverInfo->granularity) == ASE_OK)
		{
			printf("ASIOGetBufferSize (min: %d, max: %d, preferred: %d, granularity: %d);\n",
				asioDriverInfo->minSize, asioDriverInfo->maxSize,
				asioDriverInfo->preferredSize, asioDriverInfo->granularity);
			printf("we are going to use preferred size: %d and granularity: %d\n", spb, bps);
			asioDriverInfo->preferredSize = spb;
			

			// get the currently selected sample rate
			if (ASIOGetSampleRate(&asioDriverInfo->sampleRate) == ASE_OK)
			{
				printf("ASIOGetSampleRate (sampleRate: %f);\n", asioDriverInfo->sampleRate);
				printf("Used sample rate (sampleRate: %d);\n", sR);
				asioDriverInfo->sampleRate = sR;
				if (asioDriverInfo->sampleRate <= 0.0 || asioDriverInfo->sampleRate > 96000.0)
				{
					// Driver does not store it's internal sample rate, so set it to a know one.
					// Usually you should check beforehand, that the selected sample rate is valid
					// with ASIOCanSampleRate().
					if (ASIOSetSampleRate((float)sR) == ASE_OK)
					{
						if (ASIOGetSampleRate(&asioDriverInfo->sampleRate) == ASE_OK)
							printf("ASIOGetSampleRate (sampleRate: %f);\n", asioDriverInfo->sampleRate);
						else
							return -6;
					}
					else
						return -5;
				}

				// check wether the driver requires the ASIOOutputReady() optimization
				// (can be used by the driver to reduce output latency by one block)
				if (ASIOOutputReady() == ASE_OK)
					asioDriverInfo->postOutput = true;
				else
					asioDriverInfo->postOutput = false;
				printf("ASIOOutputReady(); - %s\n", asioDriverInfo->postOutput ? "Supported" : "Not supported");

				return 0;
			}
			return -3;
		}
		return -2;
	}
	return -1;
}
ASIOError createAsioBuffers(DriverInfo *asioDriverInfo, std::vector<int> iC, std::vector<int> oC)
{	// create buffers for all inputs and outputs of the card with the 
	// preferredSize from ASIOGetBufferSize() as buffer size
	long i;
	ASIOError result;

	// fill the bufferInfos from the start without a gap
	ASIOBufferInfo *info = asioDriverInfo->bufferInfos;
	
	// prepare inputs (Though this is not necessaily required, no opened inputs will work, too
	if (asioDriverInfo->inputChannels > kMaxInputChannels)
		asioDriverInfo->inputBuffers = kMaxInputChannels;
	else
		asioDriverInfo->inputBuffers = asioDriverInfo->inputChannels;
	for (i = 0; i < asioDriverInfo->inputBuffers; i++, info++)
	{
		info->isInput = ASIOTrue;
		info->channelNum = iC[i];
		info->buffers[0] = info->buffers[1] = 0;
	}
	
	// prepare outputs
	if (asioDriverInfo->outputChannels > kMaxOutputChannels)
		asioDriverInfo->outputBuffers = kMaxOutputChannels;
	else
		asioDriverInfo->outputBuffers = asioDriverInfo->outputChannels;
	for (i = 0; i < asioDriverInfo->outputBuffers; i++, info++)
	{
		info->isInput = ASIOFalse;
		info->channelNum = oC[i];
		info->buffers[0] = info->buffers[1] = 0;
	}

	// create and activate buffers
	result = ASIOCreateBuffers(asioDriverInfo->bufferInfos,
		asioDriverInfo->inputBuffers + asioDriverInfo->outputBuffers,
		asioDriverInfo->preferredSize, &asioCallbacks);
	
	if (result == ASE_OK)
	{
		
		// now get all the buffer details, sample word length, name, word clock group and activation
		for (i = 0; i < asioDriverInfo->inputBuffers + asioDriverInfo->outputBuffers; i++)
		{
			asioDriverInfo->channelInfos[i].channel = asioDriverInfo->bufferInfos[i].channelNum;
			asioDriverInfo->channelInfos[i].isInput = asioDriverInfo->bufferInfos[i].isInput;
			result = ASIOGetChannelInfo(&asioDriverInfo->channelInfos[i]);
			if (result != ASE_OK)
				break;
		}

		if (result == ASE_OK)
		{
			// get the input and output latencies
			// Latencies often are only valid after ASIOCreateBuffers()
			// (input latency is the age of the first sample in the currently returned audio block)
			// (output latency is the time the first sample in the currently returned audio block requires to get to the output)
			result = ASIOGetLatencies(&asioDriverInfo->inputLatency, &asioDriverInfo->outputLatency);
			if (result == ASE_OK)
				printf("ASIOGetLatencies (input: %d, output: %d);\n", asioDriverInfo->inputLatency, asioDriverInfo->outputLatency);
		}
	}
	return result;
}
// conversion from 64 bit ASIOSample/ASIOTimeStamp to double float
#if NATIVE_INT64
#define ASIO64toDouble(a)  (a)
#else
const double twoRaisedTo32 = 4294967296.;
#define ASIO64toDouble(a)  ((a).lo + (a).hi * twoRaisedTo32)
#endif

long asioMessages(long selector, long value, void* message, double* opt){
	return 0;
}
void sampleRateChanged(ASIOSampleRate sRate){
	// do whatever you need to do if the sample rate changed
	// usually this only happens during external sync.
	// Audio processing is not stopped by the driver, actual sample rate
	// might not have even changed, maybe only the sample rate status of an
	// AES/EBU or S/PDIF digital input at the audio device.
	// You might have to update time/sample related conversion routines, etc.
}
void buffer_delete_callback(char* data, void* hint) {  }

static void WorkAsync(uv_work_t *req) { 
// this is the worker thread, lets build up the results 
// allocated results from the heap because we'll need 
// to access in the event loop later to send back 
}
static void WorkAsyncComplete(uv_work_t *req, int status){
	Isolate * isolate = Isolate::GetCurrent(); 
	v8::HandleScope handleScope(isolate);
	Work * work = static_cast<Work *>(req->data);
	long index = work->index;
	long buffSize = asioDriverInfo.preferredSize;
	
	Local<Array> inputArr = Array::New(asioDriverInfo.isolate, buffSize*4);
	
	for (int i = 0; i < asioDriverInfo.inputBuffers + asioDriverInfo.outputBuffers; i++){
		if(asioDriverInfo.bufferInfos[i].isInput == true){
			switch (asioDriverInfo.channelInfos[i].type){
				case ASIOSTInt16LSB:
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 2, buffer_delete_callback, 0).ToLocalChecked());
					break;
				case ASIOSTInt24LSB:		// used for 20 bits as well
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 3, buffer_delete_callback, 0).ToLocalChecked());
					break;
				case ASIOSTInt32LSB:
					//printf("%d", asioDriverInfo.channelInfos[fourthOutputStream].type);
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 4, buffer_delete_callback, 0).ToLocalChecked());
					break;
			}				
		}
	
	}
	
	//pack argv
	Local<Value> argv[] = {inputArr};
	Local<Array> outputArr = Local<Array>::Cast(Local<Function>::New(asioDriverInfo.isolate, asioDriverInfo.callback)->Call(asioDriverInfo.isolate->GetCurrentContext()->Global(), 1, argv));
	// OK do processing for the outputs only
	
	for (int i = 0; i < asioDriverInfo.inputBuffers + asioDriverInfo.outputBuffers; i++){
		if(asioDriverInfo.bufferInfos[i].isInput == false){
			switch (asioDriverInfo.channelInfos[i].type){
				case ASIOSTInt16LSB:
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 2);
					break;
				case ASIOSTInt24LSB:		// used for 20 bits as well
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 3);
					break;
				case ASIOSTInt32LSB:
					//printf("%d", asioDriverInfo.channelInfos[fourthOutputStream].type);
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 4);
					break;
			}				
		}
	
	}
	
}
ASIOTime *bufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool processNow)
{	// the actual processing callback.
	// Beware that this is normally in a seperate thread, hence be sure that you take care
	// about thread synchronization. This is omitted here for simplicity.
	static long processedSamples = 0;
	
	// store the timeInfo for later use
	asioDriverInfo.tInfo = *timeInfo;

	// get the time stamp of the buffer, not necessary if no
	// synchronization to other media is required
	if (timeInfo->timeInfo.flags & kSystemTimeValid)
		asioDriverInfo.nanoSeconds = ASIO64toDouble(timeInfo->timeInfo.systemTime);
	else
		asioDriverInfo.nanoSeconds = 0;

	if (timeInfo->timeInfo.flags & kSamplePositionValid)
		asioDriverInfo.samples = ASIO64toDouble(timeInfo->timeInfo.samplePosition);
	else
		asioDriverInfo.samples = 0;

	if (timeInfo->timeCode.flags & kTcValid)
		asioDriverInfo.tcSamples = ASIO64toDouble(timeInfo->timeCode.timeCodeSamples);
	else
		asioDriverInfo.tcSamples = 0;

	// get the system reference time
	asioDriverInfo.sysRefTime = getSysReferenceTime();

#if WINDOWS && _DEBUG
	// a few debug messages for the Windows device driver developer
	// tells you the time when driver got its interrupt and the delay until the app receives
	// the event notification.
	static double last_samples = 0;
	char tmp[128];
	sprintf(tmp, "diff: %d / %d ms / %d ms / %d samples                 \n", asioDriverInfo.sysRefTime - (long)(asioDriverInfo.nanoSeconds / 1000000.0), asioDriverInfo.sysRefTime, (long)(asioDriverInfo.nanoSeconds / 1000000.0), (long)(asioDriverInfo.samples - last_samples));
	OutputDebugString(tmp);
	last_samples = asioDriverInfo.samples;
#endif
	
	// buffer size in samples
	long buffSize = asioDriverInfo.preferredSize;

	// processing for inputs send recorded data To js
	Work * work = new Work();
	work->request.data = work;
	work->index = index;
	//kicking of the worker thread
	uv_queue_work(uv_default_loop(), &work->request, WorkAsync, WorkAsyncComplete);
	//puts("assas");
	//get isolate 
	//printf("%d \n", GetCurrentThreadId());
/*
	//printf("pls work: %d\n", v8::Locker::IsActive());
	Local<Array> inputArr = Array::New(asioDriverInfo.isolate, buffSize*4);
	
	for (int i = 0; i < asioDriverInfo.inputBuffers + asioDriverInfo.outputBuffers; i++){
		if(asioDriverInfo.bufferInfos[i].isInput == true){
			switch (asioDriverInfo.channelInfos[i].type){
				case ASIOSTInt16LSB:
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 2, buffer_delete_callback, 0).ToLocalChecked());
					break;
				case ASIOSTInt24LSB:		// used for 20 bits as well
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 3, buffer_delete_callback, 0).ToLocalChecked());
					break;
				case ASIOSTInt32LSB:
					//printf("%d", asioDriverInfo.channelInfos[fourthOutputStream].type);
					inputArr->Set(i, Nan::NewBuffer((char *)asioDriverInfo.bufferInfos[i].buffers[index], buffSize * 4, buffer_delete_callback, 0).ToLocalChecked());
					break;
			}				
		}
	
	}
	
	
	//pack argv
	Local<Value> argv[] = {inputArr};
	Local<Array> outputArr = Local<Array>::Cast(Local<Function>::New(asioDriverInfo.isolate, asioDriverInfo.callback)->Call(asioDriverInfo.isolate->GetCurrentContext()->Global(), 1, argv));
	// OK do processing for the outputs only
	
	for (int i = 0; i < asioDriverInfo.inputBuffers + asioDriverInfo.outputBuffers; i++){
		if(asioDriverInfo.bufferInfos[i].isInput == false){
			switch (asioDriverInfo.channelInfos[i].type){
				case ASIOSTInt16LSB:
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 2);
					break;
				case ASIOSTInt24LSB:		// used for 20 bits as well
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 3);
					break;
				case ASIOSTInt32LSB:
					//printf("%d", asioDriverInfo.channelInfos[fourthOutputStream].type);
					memcpy(asioDriverInfo.bufferInfos[i].buffers[index], (char*) node::Buffer::Data(outputArr->Get(0)), buffSize * 4);
					break;
			}				
		}
	
	}
	*/
	/*
	switch (asioDriverInfo.channelInfos[firstOutputStream].type)
	{
	case ASIOSTInt16LSB:
		memcpy(asioDriverInfo.bufferInfos[firstOutputStream].buffers[index], asioDriverInfo.bufferInfos[firstInputStream].buffers[index], buffSize * 2);

		break;
	case ASIOSTInt24LSB:		// used for 20 bits as well
		memcpy(asioDriverInfo.bufferInfos[firstOutputStream].buffers[index], asioDriverInfo.bufferInfos[firstInputStream].buffers[index], buffSize * 3);

		break;
	case ASIOSTInt32LSB:
		//printf("%d", asioDriverInfo.channelInfos[fourthOutputStream].type);
		//memcpy(asioDriverInfo.bufferInfos[firstOutputStream].buffers[index], asioDriverInfo.bufferInfos[firstInputStream].buffers[index], buffSize * 4);
		//memcpy(asioDriverInfo.bufferInfos[secondOutputStream].buffers[index], asioDriverInfo.bufferInfos[secondInputStream].buffers[index], buffSize * 4);

		break;
	}*/
	// finally if the driver supports the ASIOOutputReady() optimization, do it here, all data are in place
	if (asioDriverInfo.postOutput)
		ASIOOutputReady();

	processedSamples += buffSize;

	return 0L;
}

//----------------------------------------------------------------------------------
void bufferSwitch(long index, ASIOBool processNow)
{	// the actual processing callback.
	// Beware that this is normally in a seperate thread, hence be sure that you take care
	// about thread synchronization. This is omitted here for simplicity.

	// as this is a "back door" into the bufferSwitchTimeInfo a timeInfo needs to be created
	// though it will only set the timeInfo.samplePosition and timeInfo.systemTime fields and the according flags
	ASIOTime  timeInfo;
	memset(&timeInfo, 0, sizeof(timeInfo));
	
	// get the time stamp of the buffer, not necessary if no
	// synchronization to other media is required
	if (ASIOGetSamplePosition(&timeInfo.timeInfo.samplePosition, &timeInfo.timeInfo.systemTime) == ASE_OK)
		timeInfo.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;

	bufferSwitchTimeInfo(&timeInfo, index, processNow);
}
unsigned long getSysReferenceTime()
{	// get the system reference time
#if WINDOWS
	return 0;
#elif MAC
	static const double twoRaisedTo32 = 4294967296.;
	UnsignedWide ys;
	Microseconds(&ys);
	double r = ((double)ys.hi * twoRaisedTo32 + (double)ys.lo);
	return (unsigned long)(r / 1000.);
#endif
}
void AsioInit(const FunctionCallbackInfo<Value>& args){
	Isolate * isolate = args.GetIsolate(); 
	Local<Object> target = args[0]->ToObject();
	Local<Object> obj = args[0]->ToObject();
	
	Local<String> prop = String::NewFromUtf8(isolate, "driver");
	Local<String> sr_prop = String::NewFromUtf8(isolate, "sampleRate");
	Local<String> bps_prop = String::NewFromUtf8(isolate, "bitsPerSample");
	Local<String> spb_prop = String::NewFromUtf8(isolate, "samplesPerBlock");
	Local<String> e_prop = String::NewFromUtf8(isolate, "endianess");
	Local<String> ic_prop = String::NewFromUtf8(isolate, "inputChannels");
	Local<String> oc_prop = String::NewFromUtf8(isolate, "outputChannels");
	
	//std::string = target->Get(prop)->
	v8::String::Utf8Value s(target->Get(prop));
	std::string driverName(*s);
	int sampleRate = target->Get(sr_prop)->Int32Value();
	int bitsPerSample = target->Get(bps_prop)->Int32Value();
	int samplesPerBlock = target->Get(spb_prop)->Int32Value();
	v8::String::Utf8Value s2(target->Get(e_prop));
	std::string endianess(*s2);
	
	//input channels
	Local<Array> inChan = Local<Array>::Cast(target->Get(ic_prop));
	std::vector<int> inputChannels;
	
	for(unsigned int i = 0; i < inChan->Length(); i++){
		inputChannels.push_back(inChan->Get(i)->Int32Value());
	}
	
	//outputChannels
	Local<Array> outChan = Local<Array>::Cast(target->Get(ic_prop));
	std::vector<int> outputChannels;
	for(unsigned int i = 0; i < outChan->Length(); i++){
		outputChannels.push_back(outChan->Get(i)->Int32Value());
	}
	// load the driver, this will setup all the necessary internal data structures
	
	char * cstrDriverName = new char[driverName.length()+1];
	strcpy(cstrDriverName, driverName.c_str());
	
	if(loadAsioDriver(cstrDriverName)){
		
		if(ASIOInit(&asioDriverInfo.driverInfo) == ASE_OK){
			printf("asioVersion:   %d\n"
				"driverVersion: %d\n"
				"Name:          %s\n"
				"ErrorMessage:  %s\n",
				asioDriverInfo.driverInfo.asioVersion, asioDriverInfo.driverInfo.driverVersion,
				asioDriverInfo.driverInfo.name, asioDriverInfo.driverInfo.errorMessage);
				
			if (initAsioDriverInfo(&asioDriverInfo,sampleRate, bitsPerSample, samplesPerBlock, endianess, inputChannels, outputChannels) == 0){
				
				asioCallbacks.bufferSwitch = &bufferSwitch;
				asioCallbacks.sampleRateDidChange = &sampleRateChanged;
				asioCallbacks.asioMessage = &asioMessages;
				asioCallbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;
				
				if(createAsioBuffers(&asioDriverInfo, inputChannels, outputChannels) == ASE_OK){

					Local<Number> retval = Int32::New(isolate, -1);
					args.GetReturnValue().Set(retval);
					return;
				}
				Local<Number> retval = Int32::New(isolate, -2);
				args.GetReturnValue().Set(retval);
				return;
			}
			Local<Number> retval = Int32::New(isolate, -3);
			args.GetReturnValue().Set(retval);
			return;
		}
		Local<Number> retval = Int32::New(isolate, -4);
		args.GetReturnValue().Set(retval);
		return;
	}
	Local<Number> retval = Int32::New(isolate, -5);
	args.GetReturnValue().Set(retval);
	return;	

}

void AsioStart(const FunctionCallbackInfo<Value>& args){
	Isolate* isolate = args.GetIsolate();
	
	// extract each location (its a list) and 
	// store it in the work package 
	// work (and thus, locations) is on the heap, 
	// accessible in the libuv threads 

	// store the callback from JS in the work 
	// package so we can invoke it later 
	Local<Function> callback = Local<Function>::Cast(args[1]); 
	Local<Array> recordedBuffers = Local<Array>::Cast(args[0]);
	asioDriverInfo.callback.Reset(isolate, callback);
	//asioDriverInfo.recordedBuffers.Reset(isolate, recordedBuffers);
	asioDriverInfo.isolate = isolate;
	printf("%d \n", GetCurrentThreadId());
	ASIOControlPanel();
	ASIOStart();
	printf("first\n");
}

void AsioStop(const FunctionCallbackInfo<Value>& args){
	ASIOStop();
	return;
}
void AsioDeInit(const FunctionCallbackInfo<Value>& args){
	ASIODisposeBuffers();
	ASIOExit();
	asioDrivers->removeCurrentDriver();
	return;
}
/*
static int unused_main(int argc, char* argv[])
{
	// load the driver, this will setup all the necessary internal data structures
	if (loadAsioDriver(ASIO_DRIVER_NAME))
	{
		// initialize the driver
		if (ASIOInit(&asioDriverInfo.driverInfo) == ASE_OK)
		{
			printf("asioVersion:   %d\n"
				"driverVersion: %d\n"
				"Name:          %s\n"
				"ErrorMessage:  %s\n",
				asioDriverInfo.driverInfo.asioVersion, asioDriverInfo.driverInfo.driverVersion,
				asioDriverInfo.driverInfo.name, asioDriverInfo.driverInfo.errorMessage);
			if (initAsioDriverInfo(&asioDriverInfo) == 0)
			{
				// ASIOControlPanel(); you might want to check wether the ASIOControlPanel() can open
				ASIOControlPanel();
				// set up the asioCallback structure and create the ASIO data buffer
				asioCallbacks.bufferSwitch = &bufferSwitch;
				asioCallbacks.sampleRateDidChange = &sampleRateChanged;
				asioCallbacks.asioMessage = &asioMessages;
				asioCallbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;
				if (createAsioBuffers(&asioDriverInfo) == ASE_OK)
				{
					if (ASIOStart() == ASE_OK)
					{
						// Now all is up and running
						fprintf(stdout, "\nASIO Driver started succefully.\n\n");
						while (!(GetAsyncKeyState('0') & 0x8000))
						{
#if WINDOWS
							Sleep(100);	// goto sleep for 100 milliseconds
#elif MAC
							unsigned long dummy;
							Delay(6, &dummy);
#endif
							//fprintf(stdout, "%d ms / %d ms / %d samples", asioDriverInfo.sysRefTime, (long)(asioDriverInfo.nanoSeconds / 1000000.0), (long)asioDriverInfo.samples);
							/*
							// create a more readable time code format (the quick and dirty way)
							double remainder = asioDriverInfo.tcSamples;
							long hours = (long)(remainder / (asioDriverInfo.sampleRate * 3600));
							remainder -= hours * asioDriverInfo.sampleRate * 3600;
							long minutes = (long)(remainder / (asioDriverInfo.sampleRate * 60));
							remainder -= minutes * asioDriverInfo.sampleRate * 60;
							long seconds = (long)(remainder / asioDriverInfo.sampleRate);
							remainder -= seconds * asioDriverInfo.sampleRate;
							fprintf(stdout, " / TC: %2.2d:%2.2d:%2.2d:%5.5d", (long)hours, (long)minutes, (long)seconds, (long)remainder);

							fprintf(stdout, "     \r");
#if !MAC
							fflush(stdout);
#endif
						}
						ASIOStop();
					}
					ASIODisposeBuffers();
				}
			}
			ASIOExit();
		}
		asioDrivers->removeCurrentDriver();
	}
	return 0;
}*/
void init(Local<Object> exports){
	NODE_SET_METHOD(exports, "initAsio", AsioInit);
	NODE_SET_METHOD(exports, "stop", AsioStop);
	NODE_SET_METHOD(exports, "deInit", AsioDeInit);
	NODE_SET_METHOD(exports, "start", AsioStart);
}
NODE_MODULE(nodeAudioAsio, init);