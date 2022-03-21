/******************************************************************************\
 * Copyright (c) 2004-2022
 *
 * Author(s):
 *  ann0see and ngocdh based on code from Volker Fischer
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
\******************************************************************************/

#pragma once
#include "soundbase.h"
#include "global.h"

#import <AudioToolbox/AudioToolbox.h>

class CSound : public CSoundBase
{
    Q_OBJECT

public:
    CSound ( void ( *fpNewProcessCallback ) ( CVector<short>& psData, void* arg ),
             void*          arg,
             const QString& strMIDISetup,
             const bool,
             const QString& );
    ~CSound();

    AudioUnit audioUnit;

    // these variables/functions should be protected but cannot since we want
    // to access them from the callback function
    //  CVector<short> vecsTmpAudioSndCrdStereo;   // Replaced by CSoundbase audioBuffer
    //  int            iCoreAudioBufferSizeMono;   // Replaced by CSoundbase iDeviceBufferSize
    //  int            iCoreAudioBufferSizeStereo; // Always use (iDeviceBufferSize * 2)
    bool isInitialized;

protected:
    //  QMutex Mutex; // Replaced by CSoundbase mutexDeviceProperties or mutexAudioProcessCallback??

    //  virtual QString LoadAndInitializeDriver ( QString strDriverName, bool ); // Replaced by checkDeviceChange(...)
    //  void            GetAvailableInOutDevices();                              // Replaced by createDeviceList ??
    //  void            SwitchDevice ( QString strDriverName );                  // Replaced by checkDeviceChange(...) ??

    AudioBuffer     buffer;
    AudioBufferList bufferList;
    void            checkStatus ( int status );
    static OSStatus recordingCallback ( void*                       inRefCon,
                                        AudioUnitRenderActionFlags* ioActionFlags,
                                        const AudioTimeStamp*       inTimeStamp,
                                        UInt32                      inBusNumber,
                                        UInt32                      inNumberFrames,
                                        AudioBufferList*            ioData );

    bool init(); // init now done by start()
    //    virtual void Start();                                      // Should use start() (called by CSoundBase)
    //    virtual void Stop();                                       // Should use stop()  (called by CSoundBase)
    // virtual void processBufferList(AudioBufferList*, CSound*);    //

    // new helpers
    bool setBaseValues();
    bool checkCapabilities();

    //========================================================================
    // pgScorpio: For clarity always list all virtual functions in separate
    //             sections at the end !
    //             In this case no Defaults! All virtuals should be abstract,
    //             so we don't forget to implemenent the neccesary virtuals
    //             in CSound
    //========================================================================
    // This Section MUST also be included in any CSound class definition !

protected: // CSoundBase Mandatory pointer to instance (must be set to 'this' in the CSound constructor)
    static CSound* pSound;

public: // CSoundBase Mandatory functions. (but static functions can't be virtual)
    static inline CSoundBase*             pInstance() { return pSound; }
    static inline const CSoundProperties& GetProperties() { return pSound->getSoundProperties(); }

protected:
    //============================================================================
    // Virtual interface to CSoundBase:
    //============================================================================
    // onChannelSelectionChanged() is only needed when selectedInputChannels[]/selectedOutputChannels[] can't be used in the process callback,
    // but normally just restarting like:
    /*
    void onChannelSelectionChanged() { Restart(); }
    */
    // should do the trick then
    virtual void onChannelSelectionChanged(){};

    virtual long         createDeviceList ( bool bRescan = false )                       = 0; // Fills strDeviceNames returns lNumDevices
    virtual bool         checkDeviceChange ( tDeviceChangeCheck mode, int iDriverIndex ) = 0; // Performs the different actions for a device change
    virtual unsigned int getDeviceBufferSize ( unsigned int iDesiredBufferSize ) = 0; // returns the nearest possible buffersize of selected device
    virtual void         closeCurrentDevice()                                    = 0; // Closes the current driver and Clears Device Info
    virtual bool         openDeviceSetup()                                       = 0; // { return false; }
    virtual bool         start()                                                 = 0; // Returns true if started, false if stopped
    virtual bool         stop()                                                  = 0; // Returns true if stopped, false if still (partly) running
};
