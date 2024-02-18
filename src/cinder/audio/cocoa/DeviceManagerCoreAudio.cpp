/*
 Copyright (c) 2014, The Cinder Project

 This code is intended to be used with the Cinder C++ library, http://libcinder.org

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
	the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
	the following disclaimer in the documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
*/

#include "cinder/audio/cocoa/ContextAudioUnit.h"
#include "cinder/audio/cocoa/DeviceManagerCoreAudio.h"
#include "cinder/cocoa/CinderCocoa.h"
#include "cinder/audio/Context.h"
#include "cinder/audio/Exception.h"
#include "cinder/CinderAssert.h"
#include "cinder/Log.h"

using namespace std;
using namespace ci;

namespace cinder { namespace audio { namespace cocoa {

// ----------------------------------------------------------------------------------------------------
// MARK: - Private AudioObject Helpers
// ----------------------------------------------------------------------------------------------------

namespace {

AudioObjectPropertyAddress getAudioObjectPropertyAddress( ::AudioObjectPropertySelector propertySelector, ::AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal )
{
	::AudioObjectPropertyAddress result;
	result.mSelector = propertySelector;
	result.mScope = scope;
	result.mElement = kAudioObjectPropertyElementMaster;
	return result;
}

UInt32 getAudioObjectPropertyDataSize( ::AudioObjectID objectId, ::AudioObjectPropertyAddress propertyAddress, UInt32 qualifierDataSize = 0, const void *qualifierData = NULL )
{
	UInt32 result = 0;
	OSStatus status = ::AudioObjectGetPropertyDataSize( objectId, &propertyAddress, qualifierDataSize, qualifierData, &result );
	if ( status != noErr ) {
		CI_LOG_W("Disconnected device detected in getAudioObjectPropertyDataSize");
		throw AudioDeviceExc("Disconnected device detected in getAudioObjectPropertyDataSize");
	}
	// CI_VERIFY( status == noErr );

	return result;
}

void getAudioObjectPropertyData( ::AudioObjectID objectId, ::AudioObjectPropertyAddress& propertyAddress, UInt32 dataSize, void *data, UInt32 qualifierDataSize = 0, const void *qualifierData = NULL )
{
	OSStatus status = ::AudioObjectGetPropertyData( objectId, &propertyAddress, qualifierDataSize, qualifierData, &dataSize, data );
	if ( status != noErr ) {
		CI_LOG_W("Disconnected device detected in getAudioObjectPropertyData");
		throw AudioDeviceExc("Disconnected device detected in getAudioObjectPropertyData");
	}
	// CI_VERIFY( status == noErr );
}

string getAudioObjectPropertyString( ::AudioObjectID objectId, ::AudioObjectPropertySelector propertySelector )
{
	::AudioObjectPropertyAddress property = getAudioObjectPropertyAddress( propertySelector );
	if( !::AudioObjectHasProperty( objectId, &property ) )
		return string();

	CFStringRef resultCF;
	UInt32 cfStringSize = sizeof( CFStringRef );

	OSStatus status = ::AudioObjectGetPropertyData( objectId, &property, 0, NULL, &cfStringSize, &resultCF );
	if ( status != noErr ) {
		CI_LOG_W("Disconnected device detected in getAudioObjectPropertyData");
		throw AudioDeviceExc("Disconnected device detected in getAudioObjectPropertyData");
	}	
	// CI_VERIFY( status == noErr );

	string result = ci::cocoa::convertCfString( resultCF );
	CFRelease( resultCF );
	return result;
}

size_t getAudioObjectNumChannels( ::AudioObjectID objectId, bool isInput )
{
	::AudioObjectPropertyAddress streamConfigProperty = getAudioObjectPropertyAddress( kAudioDevicePropertyStreamConfiguration, isInput ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput );
	UInt32 streamConfigPropertySize = getAudioObjectPropertyDataSize( objectId, streamConfigProperty );
	shared_ptr<::AudioBufferList> bufferList( (::AudioBufferList *)calloc( 1, streamConfigPropertySize ), free );

	getAudioObjectPropertyData( objectId, streamConfigProperty, streamConfigPropertySize, bufferList.get() );

	size_t numChannels = 0;
	for( int i = 0; i < bufferList->mNumberBuffers; i++ ) {
		numChannels += bufferList->mBuffers[i].mNumberChannels;
	}
	return numChannels;
}

template<typename PropT>
void setAudioObjectProperty( ::AudioObjectID objectId, ::AudioObjectPropertyAddress& propertyAddress, const PropT &data, UInt32 qualifierDataSize = 0, const void *qualifierData = NULL )
{
	UInt32 dataSize = sizeof( PropT );
	OSStatus status = ::AudioObjectSetPropertyData( objectId, &propertyAddress, qualifierDataSize, qualifierData, dataSize, &data );
	// CI_VERIFY( status == noErr );
}

template<typename PropT>
PropT getAudioObjectProperty( ::AudioObjectID objectId, ::AudioObjectPropertyAddress& propertyAddress, UInt32 qualifierDataSize = 0, const void *qualifierData = NULL )
{
	PropT result;
	UInt32 resultSize = sizeof( result );
	
	OSStatus status = ::AudioObjectGetPropertyData( objectId, &propertyAddress, qualifierDataSize, qualifierData, &resultSize, &result );
	// CI_VERIFY( status == noErr );

	return result;
}

template<typename PropT>
vector<PropT> getAudioObjectPropertyVector( ::AudioObjectID objectId, ::AudioObjectPropertySelector propertySelector, ::AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal )
{
	vector<PropT> result;
	::AudioObjectPropertyAddress propAddress = getAudioObjectPropertyAddress( propertySelector, scope );
	UInt32 propSize = getAudioObjectPropertyDataSize( objectId, propAddress );
	result.resize( propSize / sizeof( PropT ) );

	getAudioObjectPropertyData( objectId, propAddress, propSize, result.data() );
	return result;
}

} // anonymous namespace

// ----------------------------------------------------------------------------------------------------
// MARK: - DeviceManagerCoreAudio
// ----------------------------------------------------------------------------------------------------

// Callback function for device changes
OSStatus deviceChangeListener( AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress inAddresses[], void* inClientData ) {
	((DeviceManagerCoreAudio*)Context::deviceManager())->refreshDevices();
	return noErr;
}

DeviceManagerCoreAudio::DeviceManagerCoreAudio()
	: mUserHasModifiedFormat( false )
{
	// Specify the property address for the list of available audio devices
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};
	
	// Register the callback function for device changes
	::AudioObjectAddPropertyListener(
		kAudioObjectSystemObject,
		&propertyAddress,
		&deviceChangeListener,
		NULL
	);
	
	refreshDevices();
}

DeviceManagerCoreAudio::~DeviceManagerCoreAudio()
{
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	// Unregister the callback function when done
	AudioObjectRemovePropertyListener(
		kAudioObjectSystemObject,
		&propertyAddress,
		&deviceChangeListener,
		NULL
	);
}

DeviceRef DeviceManagerCoreAudio::getDefaultOutput()
{
	::AudioObjectPropertyAddress propertyAddress = getAudioObjectPropertyAddress( kAudioHardwarePropertyDefaultOutputDevice );
	auto defaultOutputId = getAudioObjectProperty<::AudioDeviceID>( kAudioObjectSystemObject, propertyAddress );
	return findDeviceByKey( DeviceManagerCoreAudio::keyForDeviceId( defaultOutputId ) );
}

DeviceRef DeviceManagerCoreAudio::getDefaultInput()
{
	::AudioObjectPropertyAddress propertyAddress = getAudioObjectPropertyAddress( kAudioHardwarePropertyDefaultInputDevice );
	auto defaultInputId = getAudioObjectProperty<::AudioDeviceID>( kAudioObjectSystemObject, propertyAddress );
	return findDeviceByKey( DeviceManagerCoreAudio::keyForDeviceId( defaultInputId ) );
}

string DeviceManagerCoreAudio::getName( const DeviceRef &device )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );
	return getAudioObjectPropertyString( deviceId, kAudioObjectPropertyName );
}

size_t DeviceManagerCoreAudio::getNumInputChannels( const DeviceRef &device )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );
	return getAudioObjectNumChannels( deviceId, true );
}

size_t DeviceManagerCoreAudio::getNumOutputChannels( const DeviceRef &device )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );
	return getAudioObjectNumChannels( deviceId, false );
}

size_t DeviceManagerCoreAudio::getSampleRate( const DeviceRef &device )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );
	::AudioObjectPropertyAddress propertyAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyNominalSampleRate );
	auto result = getAudioObjectProperty<Float64>( deviceId, propertyAddress );

	return static_cast<size_t>( result );
}

void DeviceManagerCoreAudio::setSampleRate( const DeviceRef &device, size_t sampleRate )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );

	// If the device can't be set to this sampleRate, we leave it alone,
	// users should check if sampleRate was actually updated if necessary
	auto acceptable = getAcceptableSampleRates( deviceId );
	if( find( acceptable.begin(), acceptable.end(), sampleRate ) != acceptable.end() ) {
		mUserHasModifiedFormat = true;

		::AudioObjectPropertyAddress property = getAudioObjectPropertyAddress( kAudioDevicePropertyNominalSampleRate );
		Float64 data = static_cast<Float64>( sampleRate );
		setAudioObjectProperty( deviceId, property, data );
	}
}

size_t DeviceManagerCoreAudio::getFramesPerBlock( const DeviceRef &device )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );
	::AudioObjectPropertyAddress propertyAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyBufferFrameSize );
	auto result = getAudioObjectProperty<UInt32>( deviceId, propertyAddress );

	return static_cast<size_t>( result );
}

void DeviceManagerCoreAudio::setFramesPerBlock( const DeviceRef &device, size_t framesPerBlock )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );

	auto range = getAcceptableFramesPerBlockRange( deviceId );
	if( framesPerBlock < range.first || framesPerBlock > range.second )
		throw AudioDeviceExc( "Invalid frames per block." );

	mUserHasModifiedFormat = true;

	::AudioObjectPropertyAddress property = getAudioObjectPropertyAddress( kAudioDevicePropertyBufferFrameSize );
	UInt32 data = static_cast<UInt32>( framesPerBlock );
	setAudioObjectProperty( deviceId, property, data );
}

void DeviceManagerCoreAudio::setCurrentOutputDevice( const DeviceRef &device, ::AudioComponentInstance componentInstance )
{
	setCurrentDeviceImpl( device, mCurrentOutputDevice, componentInstance, true );
	mCurrentOutputDevice = device;
}

void DeviceManagerCoreAudio::setCurrentInputDevice( const DeviceRef &device, ::AudioComponentInstance componentInstance )
{
	setCurrentDeviceImpl( device, mCurrentInputDevice, componentInstance, false );
	mCurrentInputDevice = device;
}

// ----------------------------------------------------------------------------------------------------
// MARK: - Private
// ----------------------------------------------------------------------------------------------------

void DeviceManagerCoreAudio::setCurrentDeviceImpl( const DeviceRef &device, const DeviceRef &current, ::AudioComponentInstance componentInstance, bool isOutput )
{
	::AudioDeviceID deviceId = deviceIdForDevice( device );

	if( device != current ) {
		if( current )
			unregisterPropertyListeners( current, deviceIdForDevice( current ), isOutput );

		registerPropertyListeners( device, deviceId, true );
	}

	OSStatus status = ::AudioUnitSetProperty( componentInstance, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceId, sizeof( deviceId ) );
	// CI_VERIFY( status == noErr );
}

// TODO: if device is considered 'default', register for kAudioHardwarePropertyDefaultOutputDevice and update when required
void DeviceManagerCoreAudio::registerPropertyListeners( DeviceRef device, ::AudioDeviceID deviceId, bool isOutput )
{
	// device is 'block copied' into the async callback because it is passed by value.

	AudioObjectPropertyListenerBlock listenerBlock = ^( UInt32 inNumberAddresses, const AudioObjectPropertyAddress inAddresses[] ) {

		bool paramsUpdated = false;

		for( UInt32 i = 0; i < inNumberAddresses; i++ ) {

			AudioObjectPropertyAddress propertyAddress = inAddresses[i];
			if( propertyAddress.mSelector == kAudioDevicePropertyDataSource ) {
				UInt32 dataSource = getAudioObjectProperty<UInt32>( deviceId, propertyAddress );

				::AudioObjectPropertyAddress dataSourceNameAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyDataSourceNameForIDCFString, kAudioDevicePropertyScopeOutput );;
				CFStringRef dataSourceNameCF;
				::AudioValueTranslation translation = { &dataSource, sizeof( UInt32 ), &dataSourceNameCF, sizeof( CFStringRef ) };
				getAudioObjectPropertyData( deviceId, dataSourceNameAddress, sizeof( AudioValueTranslation ), &translation );

				//string dataSourceName = ci::cocoa::convertCfString( dataSourceNameCF );
				//CFRelease( dataSourceNameCF );

				//CI_LOG_V( "device data source changed to: " << dataSourceName );
			}
			else if( propertyAddress.mSelector == kAudioDevicePropertyNominalSampleRate ) {
				paramsUpdated = true;
				//auto result = getAudioObjectProperty<Float64>( deviceId, propertyAddress );
			}
			else if( propertyAddress.mSelector == kAudioDevicePropertyBufferFrameSize ) {
				paramsUpdated = true;
				//auto result = getAudioObjectProperty<UInt32>( deviceId, propertyAddress );
			}
		}

		// only output device gets update signals
		if( isOutput && paramsUpdated ) {

			// if the change was system wide, we need to first call the will-changle signal so everything is properly uninitialized
			if( ! mUserHasModifiedFormat )
				emitParamsWillChange( device );

			emitParamsDidChange( device );
		}

		// reset user-modified flag after params-changed signals have been emitted
		mUserHasModifiedFormat = false;
	};

	dispatch_queue_t currentQueue = dispatch_get_current_queue();

	// data source (ex. internal speakers, headphones)
	::AudioObjectPropertyAddress dataSourceAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyDataSource, kAudioDevicePropertyScopeOutput );
	OSStatus status = ::AudioObjectAddPropertyListenerBlock( deviceId, &dataSourceAddress, currentQueue, listenerBlock );
	// CI_VERIFY( status == noErr );

	// device samplerate
	::AudioObjectPropertyAddress samplerateAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyNominalSampleRate );
	status = ::AudioObjectAddPropertyListenerBlock( deviceId, &samplerateAddress, currentQueue, listenerBlock );
	// CI_VERIFY( status == noErr );

	// frames per block
	::AudioObjectPropertyAddress frameSizeAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyBufferFrameSize );
	status = ::AudioObjectAddPropertyListenerBlock( deviceId, &frameSizeAddress, currentQueue, listenerBlock );
	// CI_VERIFY( status == noErr );

	if( isOutput )
		mOutputDeviceListenerBlock = Block_copy( listenerBlock );
	else
		mInputDeviceListenerBlock = Block_copy( listenerBlock );
	
	// Specify the property address for the list of available audio devices
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};
	
	// Register the callback function for device changes
	::AudioObjectAddPropertyListener(
		kAudioObjectSystemObject,
		&propertyAddress,
		&deviceChangeListener,
		NULL
	);
}

void DeviceManagerCoreAudio::unregisterPropertyListeners( const DeviceRef &device, ::AudioDeviceID deviceId, bool isOutput )
{
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	// Unregister the callback function when done
	AudioObjectRemovePropertyListener(
		kAudioObjectSystemObject,
		&propertyAddress,
		&deviceChangeListener,
		NULL
	);
	
	AudioObjectPropertyListenerBlock listenerBlock = ( isOutput ? mOutputDeviceListenerBlock : mInputDeviceListenerBlock );
	dispatch_queue_t currentQueue = dispatch_get_current_queue();

	// data source (ex. internal speakers, headphones)
	::AudioObjectPropertyAddress dataSourceAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyDataSource, kAudioDevicePropertyScopeOutput );
	OSStatus status = ::AudioObjectRemovePropertyListenerBlock( deviceId, &dataSourceAddress, currentQueue, listenerBlock );
	// still might get called for disconnected device, just to be sure
//	CI_VERIFY( status == noErr );

	// device samplerate
	::AudioObjectPropertyAddress samplerateAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyNominalSampleRate );
	status = ::AudioObjectRemovePropertyListenerBlock( deviceId, &samplerateAddress, currentQueue, listenerBlock );
//	CI_VERIFY( status == noErr );

	// frames per block
	::AudioObjectPropertyAddress frameSizeAddress = getAudioObjectPropertyAddress( kAudioDevicePropertyBufferFrameSize );
	status = ::AudioObjectRemovePropertyListenerBlock( deviceId, &frameSizeAddress, currentQueue, listenerBlock );
//	CI_VERIFY( status == noErr );

	Block_release( listenerBlock );
}
    
void DeviceManagerCoreAudio::refreshDevices()
{
	CI_LOG_I("DeviceManagerCoreAudio::refreshDevices");
	auto deviceIds = getAudioObjectPropertyVector<::AudioObjectID>( kAudioObjectSystemObject, kAudioHardwarePropertyDevices );
	vector<::AudioObjectID> addedDevices, removedDevices;
	// list devices that were newly connected
	for ( ::AudioDeviceID &deviceId : deviceIds ) {
		auto key = keyForDeviceId( deviceId );
		auto device = findDeviceByKey( key );
		if ( !device )
			addedDevices.push_back( deviceId );
	}
	// list removed devices
	for (const auto & device : mDevices) {
		if ( find_if( deviceIds.begin(), deviceIds.end(), [&device] ( auto deviceId ) {
			return std::to_string( deviceId ) == device->getKey();
		}) == deviceIds.end()) {
			removedDevices.push_back( std::stoi( device->getKey() ) );
		}
	}

	// add devices
	for ( ::AudioDeviceID &deviceId : addedDevices ) {
		CI_LOG_I("Connected device " + std::to_string(deviceId));
		string key = keyForDeviceId( deviceId );
		auto device = addDevice( key );
	}
	// removing active output device
	/*if ( mCurrentOutputDevice ) {
		if (auto it = find( removedDevices.begin(), removedDevices.end(), deviceIdForDevice( mCurrentOutputDevice ) ); it != removedDevices.end()) {
			auto newActiveDevice = mCurrentOutputDevice;
            
            // check if default output is still available. If not, switch to the first available output
            if ( find_if( deviceIds.begin(), deviceIds.end(), [] ( auto deviceId ) {
                return std::to_string( deviceId ) == Device::getDefaultOutput()->getKey();
            }) != deviceIds.end()) {
                newActiveDevice = Device::getDefaultOutput();
            }
            else {
                for ( const auto &device : Device::getOutputDevices() ) {
                    auto deviceId = deviceIdForDevice( device );
                    if (auto it = find( removedDevices.begin(), removedDevices.end(), deviceId ); it == removedDevices.end()) {
                        newActiveDevice = findDeviceByKey( keyForDeviceId( deviceId ) );
                        break;
                    }
                }
            }
            auto ctx = ci::audio::master();
			auto device = newActiveDevice;
			ci::audio::OutputDeviceNodeRef output = ctx->createOutputDeviceNode( device );
			auto outputDeviceNodeAu = dynamic_pointer_cast<OutputDeviceNodeAudioUnit>( output );
			setCurrentOutputDevice( newActiveDevice, outputDeviceNodeAu->getAudioUnit() );
		}
	}*/
    
	// remove devices
	for ( ::AudioDeviceID &deviceId : removedDevices ) {
		CI_LOG_I("Disconnected device " + std::to_string(deviceId));
		string key = keyForDeviceId( deviceId );
		if ( auto it = find_if( mDevices.begin(), mDevices.end(), [&key] ( auto device ) {
			return key == device->getKey();
		}); it != mDevices.end()) {
			mDevices.erase( it );
		}
	}
}

const vector<DeviceRef>& DeviceManagerCoreAudio::getDevices()
{
	return mDevices;
}

vector<size_t> DeviceManagerCoreAudio::getAcceptableSampleRates( ::AudioDeviceID deviceId )
{
	vector<::AudioValueRange> nsr = getAudioObjectPropertyVector<::AudioValueRange>( deviceId, kAudioDevicePropertyAvailableNominalSampleRates );
	vector<size_t> result;
	for( auto valueRange : nsr ) {
		CI_ASSERT_MSG( valueRange.mMinimum == valueRange.mMaximum, "expected min and max range to be equal" );
		result.push_back( valueRange.mMinimum );
	}

	return result;
}

pair<size_t, size_t> DeviceManagerCoreAudio::getAcceptableFramesPerBlockRange( ::AudioDeviceID deviceId )
{
	::AudioObjectPropertyAddress property = getAudioObjectPropertyAddress( kAudioDevicePropertyBufferFrameSizeRange );
	::AudioValueRange fpbRange = getAudioObjectProperty<::AudioValueRange>( deviceId, property );

	return make_pair( (size_t)fpbRange.mMinimum, (size_t)fpbRange.mMaximum );
}

// note: we cannot just rely on 'model UID', when it is there (which it isn't always), becasue it can be the same
// for two different 'devices', such as system input and output
// - current solution: key = 'NAME-[UID | MANUFACTURER]'
string DeviceManagerCoreAudio::keyForDeviceId( ::AudioDeviceID deviceId )
{
	// key should be unique, even for the same devices which is for both input and output
	return std::to_string( deviceId );
}

} } } // namespace cinder::audio::cocoa
