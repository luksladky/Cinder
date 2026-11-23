/*
 Copyright (c) 2012, The Cinder Project, All rights reserved.

 This code is intended for use with the Cinder C++ library: http://libcinder.org

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

#include "cinder/app/msw/AppImplMsw.h"
#include "cinder/app/AppBase.h"
#include "cinder/Utilities.h"
#include "cinder/Unicode.h"
#include "cinder/Display.h"
#include "cinder/msw/CinderMsw.h"
#include "cinder/app/msw/PlatformMsw.h"
#include "cinder/Log.h"

#include <Windows.h>
#include <windowsx.h>
#include <CommDlg.h>
#include <ShellAPI.h>
#include <Shlobj.h>
// The SDK declares DirectManipulation only when targeting Windows 8+, and Cinder targets 7. It is
// created at runtime and simply fails on older systems (see attachDirectManipulation()).
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN8
#include <directmanipulation.h>
#pragma pop_macro("NTDDI_VERSION")
#include <map>
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define LOSHORT(l)           ((SHORT)(l))
#define HISHORT(l)           ((SHORT)(((DWORD)(l) >> 16) & 0xFFFF))
#include <gdiplus.h>
#undef min
#undef max
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "ole32")

using std::string;
using std::wstring;
using std::vector;
using std::pair;

namespace cinder { namespace app {

LRESULT CALLBACK WndProc( HWND mWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
LRESULT CALLBACK BlankingWndProc( HWND mWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
bool isTouchpadDetectionAvailable();
static const wchar_t *WINDOWED_WIN_CLASS_NAME = TEXT("CinderWinClass");
static const wchar_t *BLANKING_WINDOW_CLASS_NAME = TEXT("CinderBlankingWindow");

// DirectManipulation: precision touchpad pan and pinch.
//
// Windows labels touchpad wheel messages IMDT_TOUCHPAD (see isTouchpadInputSource()) on some laptops
// only. Others label every gesture as a mouse (measured on an ASUS/ELAN touchpad, Windows 11) and turn
// pinch into whole Ctrl+wheel notches, so WM_MOUSEWHEEL alone cannot tell a touchpad apart there. A
// DirectManipulation viewport, the API Chrome uses, is handed the touchpad's contacts on every machine:
// the pan offset and pinch scale come straight from the OS, while mouse wheels keep arriving as
// WM_MOUSEWHEEL. Windows 10+; where creating it fails, the WM_MOUSEWHEEL path is the only one.
//
// Nothing is drawn through the viewport, only its content transform is read and turned into
// emitMouseWheel() (precise) and emitGestureMagnify(). The state is kept here, by window, rather than
// in WindowImplMsw, so the public header and apps built against it are unchanged.
//
// Each gesture is either a pinch or a scroll, never both (see DirectManipulationHandler::accumulate()):
// while the scale changes the content offset moves to keep the pinch centre in place, and after the
// fingers lift translation inertia carries that movement on. Read as a scroll, it moved the page a
// long way after every pinch.
//
// Where the touchpad's wheel messages are labelled IMDT_TOUCHPAD, the WM_MOUSEWHEEL path already
// tells pan and pinch apart, so it is kept and DirectManipulation is not used (see touchpadPath()).
namespace {

const UINT		DM_POINTERHITTEST_MSG = 0x0250;	// DM_POINTERHITTEST; the SDK declares it only for _WIN32_WINNT >= 0x0602
const UINT_PTR	DM_UPDATE_TIMER_ID = 0xC1DE;	// unique among this window's timers
const UINT		DM_UPDATE_INTERVAL_MS = 8;		// Windows rounds it up to its timer tick (~16 ms)
const int		DM_IDLE_TICKS_BEFORE_STOP = 30;	// keeps updating briefly after a contact, until the gesture starts
const float		DM_VIEWPORT_SIZE = 1000.0f;		// arbitrary, the transform is read relative to it
const DWORD		PT_TOUCHPAD_VALUE = 5;			// POINTER_INPUT_TYPE::PT_TOUCHPAD

// Deciding whether a gesture is a pinch or a scroll. Scales are compared as |ln(scale)|, so zooming in
// and out weigh the same; pans are in viewport pixels.
const float		DM_PINCH_LOCK_SCALE = 0.07f;		// ~7% of scale makes the gesture a pinch
const float		DM_PAN_SCALE_TOLERANCE = 0.02f;	// a scroll may drift this much in scale
const float		DM_PAN_LOCK_PX = 3.0f;			// a scroll this long, with the scale within tolerance, makes it a scroll
const float		DM_PAN_FORCE_LOCK_PX = 16.0f;		// a scroll this long makes it a scroll whatever the scale did
const DWORD		TOUCHPAD_PROBE_WINDOW_MS = 1000;	// a wheel message this soon after a touchpad contact came from it

//! How touchpad gestures reach the app. Decided once per process from the first touchpad gesture.
enum class TouchpadPath { Unknown, Wheel, DirectManipulation };
TouchpadPath	sTouchpadPath = TouchpadPath::Unknown;
bool			sTouchpadPathInitialized = false;
DWORD			sTouchpadContactTick = 0;
bool			sTouchpadContactPending = false;

const char* touchpadPathName( TouchpadPath path )
{
	switch( path ) {
		case TouchpadPath::Wheel: return "wheel messages";
		case TouchpadPath::DirectManipulation: return "DirectManipulation";
		default: return "unknown";
	}
}

//! Unknown until the first touchpad gesture, unless the CINDER_MSW_TOUCHPAD environment variable
//! ("wheel" or "dm") picks it, or Windows cannot label messages at all (before 10 1703).
TouchpadPath touchpadPath()
{
	if( ! sTouchpadPathInitialized ) {
		sTouchpadPathInitialized = true;
		const char *env = std::getenv( "CINDER_MSW_TOUCHPAD" );
		if( env && std::string( env ) == "wheel" )
			sTouchpadPath = TouchpadPath::Wheel;
		else if( env && std::string( env ) == "dm" )
			sTouchpadPath = TouchpadPath::DirectManipulation;
		else if( ! isTouchpadDetectionAvailable() )
			sTouchpadPath = TouchpadPath::DirectManipulation;
		if( sTouchpadPath != TouchpadPath::Unknown )
			CI_LOG_I( "touchpad: using " << touchpadPathName( sTouchpadPath ) << ( env ? " (CINDER_MSW_TOUCHPAD)" : "" ) );
	}
	return sTouchpadPath;
}

//! Called for every wheel message while the path is unknown. Until then touchpad contacts are left to
//! Windows, which turns them into wheel messages; how those are labelled decides the path. A labelled
//! one means WM_MOUSEWHEEL can tell the touchpad apart. An unlabelled one right after a touchpad
//! contact means the touchpad is reported as a mouse, so its gestures go to DirectManipulation.
void observeTouchpadWheel( bool labelledTouchpad )
{
	if( touchpadPath() != TouchpadPath::Unknown )
		return;
	if( labelledTouchpad )
		sTouchpadPath = TouchpadPath::Wheel;
	else if( sTouchpadContactPending && ::GetTickCount() - sTouchpadContactTick < TOUCHPAD_PROBE_WINDOW_MS )
		sTouchpadPath = TouchpadPath::DirectManipulation;
	else
		return;
	sTouchpadContactPending = false;
	CI_LOG_I( "touchpad: wheel messages " << ( labelledTouchpad ? "are" : "are not" ) << " labelled as touchpad, using " << touchpadPathName( sTouchpadPath ) );
}

typedef BOOL (WINAPI *GetPointerTypeFn)( UINT32, DWORD* );

//! Resolves GetPointerType() (Windows 8+) once, like getInputMessageSourceFn() below.
GetPointerTypeFn getPointerTypeFn()
{
	static bool sResolved = false;
	static GetPointerTypeFn sFn = nullptr;
	if( ! sResolved ) {
		sResolved = true;
		HMODULE user32 = ::GetModuleHandle( TEXT("user32.dll") );
		if( user32 )
			*(size_t *)&sFn = (size_t)::GetProcAddress( user32, "GetPointerType" );
	}
	return sFn;
}

//! Collects what the viewport reports during IDirectManipulationUpdateManager::Update().
class DirectManipulationHandler : public IDirectManipulationViewportEventHandler {
  public:
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void **out ) override
	{
		if( riid == __uuidof(IUnknown) || riid == __uuidof(IDirectManipulationViewportEventHandler) ) {
			*out = static_cast<IDirectManipulationViewportEventHandler*>( this );
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override	{ return (ULONG)::InterlockedIncrement( &mRefCount ); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const LONG count = ::InterlockedDecrement( &mRefCount );
		if( count == 0 )
			delete this;
		return (ULONG)count;
	}

	HRESULT STDMETHODCALLTYPE OnViewportStatusChanged( IDirectManipulationViewport *viewport, DIRECTMANIPULATION_STATUS current, DIRECTMANIPULATION_STATUS previous ) override
	{
		if( mResetting ) // the reset below reports status changes of its own
			return S_OK;
		mActive = ( current == DIRECTMANIPULATION_RUNNING ) || ( current == DIRECTMANIPULATION_INERTIA );
		// A gesture ends in READY, or in RUNNING again when the fingers land during inertia; the next one
		// picks its mode afresh.
		if( current == DIRECTMANIPULATION_READY || ( current == DIRECTMANIPULATION_RUNNING && previous != DIRECTMANIPULATION_RUNNING ) )
			endGesture();
		// Back to identity, so every gesture starts at scale 1 and offset 0 (as Chrome does). The content
		// moves there on a later Update(), and that move is not a gesture: read as one, it scrolled the
		// whole gesture back the other way on release. The move ends in READY again; resetting then
		// would wait for an update that never comes, as the content is already there.
		if( current == DIRECTMANIPULATION_READY && ! isAtIdentity() ) {
			mResetPending = true;
			mResetting = true;
			viewport->ZoomToRect( 0, 0, DM_VIEWPORT_SIZE, DM_VIEWPORT_SIZE, FALSE );
			mResetting = false;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnViewportUpdated( IDirectManipulationViewport* ) override	{ return S_OK; }

	HRESULT STDMETHODCALLTYPE OnContentUpdated( IDirectManipulationViewport*, IDirectManipulationContent *content ) override
	{
		float m[6];
		if( FAILED( content->GetContentTransform( m, 6 ) ) || m[0] <= 0 )
			return S_OK;
		const float scale = m[0];
		const vec2 offset( m[4], m[5] );
		if( ! mResetPending && ! mResetting ) {
			// while the scale changes, the offset only keeps the pinch centre in place; it is not a scroll
			if( scale != mScale )
				accumulate( scale / mScale, vec2( 0 ) );
			else
				accumulate( 1, offset - mOffset );
		}
		mScale = scale;
		mOffset = offset;
		if( mResetPending && isAtIdentity() ) // only follow the content until the reset lands
			mResetPending = false;
		return S_OK;
	}

	bool	isAtIdentity() const
	{
		return std::abs( mScale - 1.0f ) < 1e-4f && std::abs( mOffset.x ) < 0.5f && std::abs( mOffset.y ) < 0.5f;
	}

	//! True from the first finger movement until inertia ends.
	bool	isActive() const	{ return mActive; }

	//! Returns and clears the scale ratio (1 = none) and the pan in pixels gathered since the last call.
	void	takePending( float *scale, vec2 *pan )
	{
		*scale = mPendingScale;
		*pan = mPendingPan;
		mPendingScale = 1;
		mPendingPan = vec2( 0 );
	}

  private:
	enum class Mode { Undecided, Pinch, Pan };

	//! Passes the gesture's scale or its pan on, never both. Until one of them is clearly meant, both
	//! are held back. A pinch then passes on the scale held back, so it zooms from where the fingers
	//! started; a scroll starts from the point it was recognised, as on iOS, so it starts smoothly
	//! instead of with a jump. The other one is dropped for the rest of the gesture, inertia included.
	void	accumulate( float scaleRatio, const vec2 &pan )
	{
		if( mMode == Mode::Pinch ) {
			mPendingScale *= scaleRatio;
			return;
		}
		if( mMode == Mode::Pan ) {
			mPendingPan += pan;
			return;
		}

		mUndecidedScale *= scaleRatio;
		mUndecidedPan += pan;
		const float scaleChange = std::abs( std::log( mUndecidedScale ) );
		const float panLength = glm::length( mUndecidedPan );
		if( scaleChange >= DM_PINCH_LOCK_SCALE )
			mMode = Mode::Pinch;
		else if( ( panLength >= DM_PAN_LOCK_PX && scaleChange <= DM_PAN_SCALE_TOLERANCE ) || panLength >= DM_PAN_FORCE_LOCK_PX )
			mMode = Mode::Pan;
		else
			return;

		CI_LOG_V( "touchpad gesture: " << ( mMode == Mode::Pinch ? "pinch" : "scroll" ) << " (scale " << mUndecidedScale << ", pan " << panLength << " px)" );
		if( mMode == Mode::Pinch )
			mPendingScale *= mUndecidedScale;
		mUndecidedScale = 1;
		mUndecidedPan = vec2( 0 );
	}

	//! A gesture that was never recognised does nothing.
	void	endGesture()
	{
		mMode = Mode::Undecided;
		mUndecidedScale = 1;
		mUndecidedPan = vec2( 0 );
	}

	LONG	mRefCount = 1;
	Mode	mMode = Mode::Undecided;
	float	mUndecidedScale = 1;
	vec2	mUndecidedPan = vec2( 0 );
	bool	mResetting = false;
	bool	mResetPending = false;	// reset requested, content not back at identity yet
	bool	mActive = false;
	float	mScale = 1;
	float	mPendingScale = 1;
	vec2	mOffset = vec2( 0 );
	vec2	mPendingPan = vec2( 0 );
};

struct DirectManipulation {
	IDirectManipulationManager			*manager = nullptr;
	IDirectManipulationUpdateManager	*updateManager = nullptr;
	IDirectManipulationViewport			*viewport = nullptr;
	DirectManipulationHandler			*handler = nullptr;
	DWORD								handlerCookie = 0;
	int									idleTicks = 0;
};

std::map<HWND, DirectManipulation> sDirectManipulation;

void releaseDirectManipulation( HWND wnd, DirectManipulation &dm )
{
	if( dm.viewport ) {
		dm.viewport->Stop();
		if( dm.handlerCookie )
			dm.viewport->RemoveEventHandler( dm.handlerCookie );
		dm.viewport->Release();
	}
	if( dm.handler )
		dm.handler->Release();
	if( dm.updateManager )
		dm.updateManager->Release();
	if( dm.manager ) {
		dm.manager->Deactivate( wnd );
		dm.manager->Release();
	}
	dm = DirectManipulation();
}

void attachDirectManipulation( HWND wnd )
{
	cinder::msw::initializeCom();

	const DIRECTMANIPULATION_CONFIGURATION config = DIRECTMANIPULATION_CONFIGURATION_INTERACTION
		| DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X | DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y
		| DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_INERTIA
		| DIRECTMANIPULATION_CONFIGURATION_RAILS_X | DIRECTMANIPULATION_CONFIGURATION_RAILS_Y
		| DIRECTMANIPULATION_CONFIGURATION_SCALING;
	RECT rect = { 0, 0, (LONG)DM_VIEWPORT_SIZE, (LONG)DM_VIEWPORT_SIZE };

	DirectManipulation dm;
	bool ok = SUCCEEDED( ::CoCreateInstance( __uuidof(DirectManipulationManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &dm.manager ) ) )
		&& SUCCEEDED( dm.manager->GetUpdateManager( IID_PPV_ARGS( &dm.updateManager ) ) )
		&& SUCCEEDED( dm.manager->CreateViewport( nullptr, wnd, IID_PPV_ARGS( &dm.viewport ) ) )
		&& SUCCEEDED( dm.viewport->ActivateConfiguration( config ) )
		&& SUCCEEDED( dm.viewport->SetViewportOptions( DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE ) );
	if( ok ) {
		dm.handler = new DirectManipulationHandler;
		ok = SUCCEEDED( dm.viewport->AddEventHandler( wnd, dm.handler, &dm.handlerCookie ) )
			&& SUCCEEDED( dm.viewport->SetViewportRect( &rect ) )
			&& SUCCEEDED( dm.manager->Activate( wnd ) )
			&& SUCCEEDED( dm.viewport->Enable() );
	}
	if( ! ok ) {
		releaseDirectManipulation( wnd, dm );
		return;
	}

	dm.updateManager->Update( nullptr );
	sDirectManipulation[wnd] = dm;
}

void detachDirectManipulation( HWND wnd )
{
	auto it = sDirectManipulation.find( wnd );
	if( it == sDirectManipulation.end() )
		return;
	::KillTimer( wnd, DM_UPDATE_TIMER_ID );
	releaseDirectManipulation( wnd, it->second );
	sDirectManipulation.erase( it );
}

//! Hands a touchpad contact to the viewport and starts updating it. Returns false for any other
//! pointer (touch screen, pen), which keeps its default handling.
bool directManipulationHitTest( HWND wnd, WPARAM wParam )
{
	auto it = sDirectManipulation.find( wnd );
	GetPointerTypeFn getPointerType = getPointerTypeFn();
	if( it == sDirectManipulation.end() || ! getPointerType )
		return false;

	const UINT32 pointerId = LOWORD( wParam );	// GET_POINTERID_WPARAM
	DWORD type = 0;
	if( ! (*getPointerType)( pointerId, &type ) || type != PT_TOUCHPAD_VALUE )
		return false;
	// Left to Windows, the contact arrives as wheel messages, which tell how it reports the touchpad
	// (see observeTouchpadWheel()).
	sTouchpadContactTick = ::GetTickCount();
	sTouchpadContactPending = true;
	if( touchpadPath() != TouchpadPath::DirectManipulation )
		return false;
	if( FAILED( it->second.viewport->SetContact( pointerId ) ) )
		return false;

	it->second.idleTicks = 0;
	::SetTimer( wnd, DM_UPDATE_TIMER_ID, DM_UPDATE_INTERVAL_MS, nullptr );
	return true;
}

//! Advances the viewport by one frame and returns the pinch scale ratio and pan in pixels that it
//! produced. Stops the timer once the gesture, inertia included, is over.
void directManipulationUpdate( HWND wnd, float *scale, vec2 *pan )
{
	*scale = 1;
	*pan = vec2( 0 );
	auto it = sDirectManipulation.find( wnd );
	if( it == sDirectManipulation.end() ) {
		::KillTimer( wnd, DM_UPDATE_TIMER_ID );
		return;
	}

	DirectManipulation &dm = it->second;
	dm.updateManager->Update( nullptr );
	dm.handler->takePending( scale, pan );
	if( dm.handler->isActive() )
		dm.idleTicks = 0;
	else if( ++dm.idleTicks > DM_IDLE_TICKS_BEFORE_STOP )
		::KillTimer( wnd, DM_UPDATE_TIMER_ID );
}

//! Mouse modifiers from the keyboard state, for events that do not come with a WPARAM of MK_ flags.
unsigned int currentMouseModifiers()
{
	unsigned int result = 0;
	if( ::GetKeyState( VK_CONTROL ) < 0 ) result |= MouseEvent::CTRL_DOWN;
	if( ::GetKeyState( VK_SHIFT ) < 0 ) result |= MouseEvent::SHIFT_DOWN;
	if( ::GetKeyState( VK_MENU ) < 0 ) result |= MouseEvent::ALT_DOWN;
	if( (::GetKeyState( VK_LWIN ) < 0) || (::GetKeyState( VK_RWIN ) < 0) ) result |= MouseEvent::META_DOWN;
	if( ::GetKeyState( VK_LBUTTON ) < 0 ) result |= MouseEvent::LEFT_DOWN;
	if( ::GetKeyState( VK_MBUTTON ) < 0 ) result |= MouseEvent::MIDDLE_DOWN;
	if( ::GetKeyState( VK_RBUTTON ) < 0 ) result |= MouseEvent::RIGHT_DOWN;
	return result;
}

} // anonymous namespace

AppImplMsw::AppImplMsw( AppBase *aApp )
	: mApp( aApp ), mSetupHasBeenCalled( false ), mActive( true ), mNeedsToRefreshDisplays( false )
{
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup( &mGdiplusToken, &gdiplusStartupInput, NULL );
}

AppImplMsw::~AppImplMsw()
{
	// there's no way to ensure all GDI+ objects have been freed, so we don't do this
	// for now this seems fine
//	Gdiplus::GdiplusShutdown( mGdiplusToken );
}

void AppImplMsw::hideCursor()
{
	int counter;

	while( ( counter = ::ShowCursor( false ) ) > -1 );

	// when repeatedly calling hideCursor(), keep counter at -1
	if( counter < -1 )
		while( ::ShowCursor( true ) < -1 );
}

void AppImplMsw::showCursor()
{
	int counter;

	while( ( counter = ::ShowCursor( true ) ) < 0 );

	// when repeatedly calling showCursor(), keep counter at 0
	if( counter > 0 )
		while( ::ShowCursor( false ) > 0 );
}

fs::path AppImplMsw::getOpenFilePath( const fs::path &initialPath, vector<string> extensions )
{
	OPENFILENAMEW ofn;       // common dialog box structure
	wchar_t szFile[MAX_PATH];       // buffer for file name
	wchar_t extensionStr[10000];
	wchar_t initialPathStr[MAX_PATH];

	// Initialize OPENFILENAME
	::ZeroMemory( &ofn, sizeof(ofn) );
	ofn.lStructSize = sizeof(ofn);
	auto app = AppBase::get();
	if( app && app->getRenderer() )
		ofn.hwndOwner = app->getRenderer()->getHwnd();
	else
		ofn.hwndOwner = 0;
	ofn.lpstrFile = szFile;
	
	// Set lpstrFile[0] to '\0' so that GetOpenFileName does not
	// use the contents of szFile to initialize itself.
	//
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof( szFile );
	if( extensions.empty() ) {
		ofn.lpstrFilter = L"All\0*.*\0";
	}
	else {
		size_t offset = 0;
		
		wcscpy( extensionStr, L"Supported Types" );
		offset += wcslen( extensionStr ) + 1;
		for( vector<string>::const_iterator strIt = extensions.begin(); strIt != extensions.end(); ++strIt ) {
			wcscpy( extensionStr + offset, L"*." );
			offset += 2;
			wcscpy( extensionStr + offset, msw::toWideString( *strIt ).c_str() );
			offset += strIt->length();
			// append a semicolon to all but the last extensions
			if( strIt + 1 != extensions.end() ) {
				extensionStr[offset] = L';';
				offset += 1;
			}
			else {
				extensionStr[offset] = L'\0';
				offset += 1;
			}
		}

		extensionStr[offset] = 0;
		ofn.lpstrFilter = extensionStr;
	}
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	if( initialPath.empty() ) {
		ofn.lpstrInitialDir = NULL;
	}
	else if( fs::is_directory( initialPath ) ) {
		wcscpy( initialPathStr, initialPath.wstring().c_str() );
		ofn.lpstrInitialDir = initialPathStr;
	}
	else {
		wcscpy( initialPathStr, initialPath.parent_path().wstring().c_str() );
		ofn.lpstrInitialDir = initialPathStr;
		wcscpy( szFile, initialPath.filename().wstring().c_str() );
	}
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	// Display the Open dialog box.
	if( ::GetOpenFileNameW( &ofn ) == TRUE ) {
		return fs::path( ofn.lpstrFile );
	}
	else
		return fs::path();
}

namespace {

// see http://support.microsoft.com/kb/179378 "How To Browse for Folders from the Current Directory"
INT CALLBACK getFolderPathBrowseCallbackProc( HWND hwnd, UINT uMsg, LPARAM /* lp */, LPARAM pData )
{
	switch( uMsg ) {
		case BFFM_INITIALIZED:
			// WParam is TRUE since you are passing a path.
			// It would be FALSE if you were passing a pidl.
			// pData is a pointer to the wide string containing our initial path back at the original call site
			::SendMessage( hwnd, BFFM_SETSELECTION, TRUE, pData );
		break;
	}
	return 0;
}

} // anonymous namespace

fs::path AppImplMsw::getFolderPath( const fs::path &initialPath )
{
	fs::path result;

	::BROWSEINFO bi = { 0 };
	bi.lParam = reinterpret_cast<LPARAM>( initialPath.c_str() );
	bi.lpfn = getFolderPathBrowseCallbackProc;
	bi.lpszTitle = L"Pick a Directory";
	::LPITEMIDLIST pidl = ::SHBrowseForFolder( &bi );
	if( pidl ) {
		// get the name of the folder
		TCHAR path[MAX_PATH];
		if( ::SHGetPathFromIDList( pidl, path ) ) {
			result = path;
		}

		// free memory used
		::IMalloc * imalloc = 0;
		if( SUCCEEDED( ::SHGetMalloc( &imalloc ) ) ) {
			imalloc->Free( pidl );
			imalloc->Release();
		}
	}

	return result;
}

bool AppImplMsw::getHighDensityDisplayEnabled() const
{
	return mApp->isHighDensityDisplayEnabled();
}

fs::path AppImplMsw::getSaveFilePath( const fs::path &initialPath, vector<string> extensions )
{
	OPENFILENAMEW ofn;       // common dialog box structure
	wchar_t szFile[MAX_PATH];       // buffer for file name
	wchar_t initialPathStr[MAX_PATH];
	wchar_t extensionStr[10000];

	// Initialize OPENFILENAME
	ZeroMemory( &ofn, sizeof(ofn) );
	ofn.lStructSize = sizeof(ofn);
	auto app = AppBase::get();
	if( app && app->getRenderer() )
		ofn.hwndOwner = app->getRenderer()->getHwnd();
	else
		ofn.hwndOwner = 0;
	ofn.lpstrFile = szFile;

	// Set lpstrFile[0] to '\0' so that GetSaveFileName does not
	// use the contents of szFile to initialize itself.
	//
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof( szFile );
	if( extensions.empty() ) {
		ofn.lpstrFilter = L"All\0*.*\0";
	}
	else {
		size_t offset = 0;

		wcscpy( extensionStr, L"Supported Types" );
		offset += wcslen( extensionStr ) + 1;
		for( vector<string>::const_iterator strIt = extensions.begin(); strIt != extensions.end(); ++strIt ) {
			wcscpy( extensionStr + offset, L"*." );
			offset += 2;
			wcscpy( extensionStr + offset, msw::toWideString( strIt->c_str() ).c_str() );
			offset += strIt->length();
			// append a semicolon to all but the last extensions
			if( strIt + 1 != extensions.end() ) {
				extensionStr[offset] = L';';
				offset += 1;
			}
			else {
				extensionStr[offset] = 0;
				offset += 1;
			}
		}

		extensionStr[offset] = 0;
		ofn.lpstrFilter = extensionStr;
	}
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	if( initialPath.empty() ) {
		ofn.lpstrInitialDir = NULL;
	}
	else if( fs::is_directory( initialPath ) ) {
		wcscpy( initialPathStr, initialPath.wstring().c_str() );
		ofn.lpstrInitialDir = initialPathStr;
	}
	else {
		wcscpy( initialPathStr, initialPath.parent_path().wstring().c_str() );
		ofn.lpstrInitialDir = initialPathStr;
		wcscpy( szFile, initialPath.filename().wstring().c_str() );
	}
	ofn.Flags = OFN_SHOWHELP | OFN_OVERWRITEPROMPT;

	// Display the Open dialog box.
	if( ::GetSaveFileName( &ofn ) == TRUE )
		return fs::path( ofn.lpstrFile );
	else
		return fs::path();
}


///////////////////////////////////////////////////////////////////////////////
// WindowImplMsw
WindowImplMsw::WindowImplMsw( const Window::Format &format, RendererRef sharedRenderer, AppImplMsw *appImpl )
	: mWindowOffset( 0, 0 ), mAppImpl( appImpl ), mIsDragging( false ), mHidden( false )
{	
	mFullScreen = format.isFullScreen();
	mDisplay = format.getDisplay();
	mRenderer = format.getRenderer();
	mResizable = format.isResizable();
	mAlwaysOnTop = format.isAlwaysOnTop();
	mBorderless = format.isBorderless();

	if( ! mDisplay )
		mDisplay = Display::getMainDisplay();

	if( appImpl->getHighDensityDisplayEnabled() )
		mWindowedSizePx = ivec2( vec2( format.getSize() ) * mDisplay->getContentScale() );
	else
		mWindowedSizePx = format.getSize();		
	mWindowWidthPx = mWindowedSizePx.x;
	mWindowHeightPx = mWindowedSizePx.y;
	if( format.isPosSpecified() ) {
		mWindowOffset = mWindowedPos = mDisplay->getBounds().getUL() + format.getPos();
	}
	else {
		ivec2 displaySize = mDisplay->getSize();
		mWindowOffset = mWindowedPos = mDisplay->getBounds().getUL() + ( displaySize - mWindowedSizePx ) / 2;
	}

	createWindow( ivec2( mWindowWidthPx, mWindowHeightPx ), format.getTitle(), mDisplay, sharedRenderer );
	// set WindowRef and its impl pointer to this
	mWindowRef = Window::privateCreate__( this, mAppImpl->getApp() );
	
	completeCreation();
}

WindowImplMsw::WindowImplMsw( HWND hwnd, RendererRef renderer, RendererRef sharedRenderer, AppImplMsw *appImpl )
	: mWnd( hwnd ), mRenderer( renderer ), mAppImpl( appImpl ), mIsDragging( false )
{
	RECT rect;
	::GetWindowRect( mWnd, &rect );
	
	mDC = ::GetDC( hwnd );
	mWindowOffset = ivec2( rect.left, rect.top );
	mWindowWidthPx = rect.right - rect.left;
	mWindowHeightPx = rect.bottom - rect.top;

	mDisplay = app::PlatformMsw::get()->findDisplayFromHmonitor( ::MonitorFromWindow( mWnd, MONITOR_DEFAULTTONEAREST ) );

	mRenderer->setup( this, sharedRenderer );

	mWindowRef = Window::privateCreate__( this, mAppImpl->getApp() );
}

void WindowImplMsw::setWindowStyleValues()
{
	if( mFullScreen ) {
		mWindowExStyle = WS_EX_APPWINDOW;								// Window Extended Style
		mWindowStyle = WS_POPUP;										// Windows Style
	}
	else if( mBorderless ) {
		mWindowExStyle = WS_EX_APPWINDOW;
		mWindowStyle = WS_POPUP;
	}
	else {
		mWindowExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_ACCEPTFILES;				// Window Extended Style
		mWindowStyle = ( mResizable ) ? WS_OVERLAPPEDWINDOW
			:	( WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX );	// Windows Style
	}
}

void WindowImplMsw::createWindow( const ivec2 &windowSize, const std::string &title, const DisplayRef display, RendererRef sharedRenderer )
{
	RECT windowRect;
	Area displayArea = display->getBounds();

	if( mFullScreen ) {
		windowRect.left = displayArea.getX1();
		windowRect.right = displayArea.getX2();
		windowRect.top = displayArea.getY1();
		windowRect.bottom = displayArea.getY2();
	}
	else {
		windowRect.left = mWindowedPos.x; 
		windowRect.right = mWindowedPos.x + windowSize.x;
		windowRect.top = mWindowedPos.y;
		windowRect.bottom = mWindowedPos.y + windowSize.y;
	}

	registerWindowClass();
	setWindowStyleValues();
	::AdjustWindowRectEx( &windowRect, mWindowStyle, FALSE, mWindowExStyle );		// Adjust Window To True Requested Size

	std::wstring wideTitle = msw::toWideString( title ); 

	// Create The Window
	if( 0 == ( mWnd = ::CreateWindowEx( mWindowExStyle,						// Extended Style For The Window
		WINDOWED_WIN_CLASS_NAME,
		wideTitle.c_str(),						// Window Title
		mWindowStyle,					// Required Window Style
		windowRect.left, windowRect.top,								// Window Position
		windowRect.right - windowRect.left,	// Calculate Window Width
		windowRect.bottom - windowRect.top,	// Calculate Window Height
		NULL,								// No Parent Window
		NULL,								// No Menu
		::GetModuleHandle( NULL ),
		reinterpret_cast<LPVOID>( this ) )) )

	{
		//killWindow();							// Reset The Display
		return;		
	}

	mDC = ::GetDC( mWnd );
	if( ! mDC ) {
		return;
	}

	if( mAlwaysOnTop ) {
		::SetWindowLongA( mWnd, GWL_EXSTYLE, ::GetWindowLongA( mWnd, GWL_EXSTYLE ) | WS_EX_TOPMOST );
		::SetWindowPos( mWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE );
	}

	// update display
	mDisplay = PlatformMsw::get()->findDisplayFromHmonitor( ::MonitorFromWindow( mWnd, MONITOR_DEFAULTTONEAREST ) );

	mRenderer->setup( this, sharedRenderer );
}

void WindowImplMsw::completeCreation()
{
	::DragAcceptFiles( mWnd, TRUE );
	if( mAppImpl->mApp->isMultiTouchEnabled() )
		enableMultiTouch();
	attachDirectManipulation( mWnd );

	::ShowWindow( mWnd, SW_SHOW );
	::SetForegroundWindow( mWnd );
	::SetFocus( mWnd );
}

void WindowImplMsw::registerWindowClass()
{
	static bool sRegistered = false;

	if( sRegistered )
		return;

	WNDCLASSEX	wc;
	HMODULE instance	= ::GetModuleHandle( NULL );				// Grab An Instance For Our Window
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;	// Redraw On Size, And Own DC For Window.
	wc.lpfnWndProc		= WndProc;						// WndProc Handles Messages
	wc.cbClsExtra		= 0;									// No Extra Window Data
	wc.cbWndExtra		= 0;									// No Extra Window Data
	wc.hInstance		= instance;							// Set The Instance
	wc.hIcon = (HICON)::LoadImage( instance, MAKEINTRESOURCE(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE ); // Load The Default Cinder Icon
	wc.hIconSm = NULL;
	wc.hCursor			= ::LoadCursor( NULL, IDC_ARROW );		// Load The Arrow Pointer
	wc.hbrBackground	= NULL;									// No Background Required For GL
	wc.lpszMenuName		= NULL;									// We Don't Want A Menu
	wc.lpszClassName	= WINDOWED_WIN_CLASS_NAME;

	if( ! ::RegisterClassEx( &wc ) ) {								// Attempt To Register The Window Class
		DWORD err = ::GetLastError();
		return;							
	}

	sRegistered = true;
}

void WindowImplMsw::setFullScreen( bool fullScreen, const app::FullScreenOptions &options )
{
	if( mFullScreen != fullScreen )
		toggleFullScreen( options );
}

void WindowImplMsw::toggleFullScreen( const app::FullScreenOptions &options )
{
	ivec2 newWindowSize;
	bool prevFullScreen = mFullScreen;
	
	mFullScreen = ! mFullScreen;
	setWindowStyleValues();

	RECT windowRect;
	if( prevFullScreen ) {
		windowRect.left = mWindowedPos.x;
		windowRect.top = mWindowedPos.y;
		windowRect.right = mWindowedPos.x + mWindowedSizePx.x;
		windowRect.bottom = mWindowedPos.y + mWindowedSizePx.y;
		::AdjustWindowRectEx( &windowRect, mWindowStyle, FALSE, mWindowExStyle );
	}
	else {
		DisplayRef display = options.getDisplay();
		if( ! display ) // use the default, which is this Window's display
			display = mDisplay;

		mWindowedPos = mWindowOffset;
		mWindowedSizePx = ivec2( mWindowWidthPx, mWindowHeightPx );
		newWindowSize = display->getSize();

		Area area = display->getBounds();

		windowRect.left = area.getX1();
		windowRect.top = area.getY1();
		windowRect.right = area.getX2();
		windowRect.bottom = area.getY2();
	}

	::SetWindowLongA( mWnd, GWL_STYLE, mWindowStyle );
	::SetWindowLongA( mWnd, GWL_EXSTYLE, mWindowExStyle );
	::SetWindowPos( mWnd, HWND_TOP, windowRect.left, windowRect.top, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
			SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOCOPYBITS|SWP_NOREDRAW );

	::ShowWindow( mWnd, SW_SHOW );
	::SetForegroundWindow( mWnd );
	::SetFocus( mWnd );
}

void WindowImplMsw::getScreenSize( int clientWidth, int clientHeight, int *resultWidth, int *resultHeight )
{
	RECT windowRect;
	windowRect.left = windowRect.top = 0;
	windowRect.right = clientWidth;
	windowRect.bottom = clientHeight;
	::AdjustWindowRectEx( &windowRect, mWindowStyle, FALSE, mWindowExStyle );
	*resultWidth = windowRect.right - windowRect.left;
	*resultHeight = windowRect.bottom - windowRect.top;
}

void WindowImplMsw::setPos( const ivec2 &windowPos )
{
	RECT clientArea;
	clientArea.left = windowPos.x * getContentScale();
	clientArea.top = windowPos.y * getContentScale();
	clientArea.right = windowPos.x + 1; clientArea.bottom = windowPos.y + 1; // we don't actually care about the lower-right
	::AdjustWindowRectEx( &clientArea, mWindowStyle, FALSE, mWindowExStyle );
	::SetWindowPos( mWnd, HWND_TOP, clientArea.left, clientArea.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
	
	POINT upperLeft;
	upperLeft.x = upperLeft.y = 0;
	::ClientToScreen( mWnd, &upperLeft );
	mWindowOffset.x = upperLeft.x;
	mWindowOffset.y = upperLeft.y;
}

void WindowImplMsw::hide()
{
	::ShowWindow( mWnd, SW_HIDE );
	mHidden = true;
}

void WindowImplMsw::show()
{
	::ShowWindow( mWnd, SW_SHOW );
	mHidden = false;
}

bool WindowImplMsw::isHidden() const
{
	return mHidden;
}

std::string	WindowImplMsw::getTitle() const
{
	int numChars = ::GetWindowTextLengthW( mWnd );
	wchar_t *wideChars = (wchar_t*)malloc( sizeof(wchar_t) * (numChars + 1) );
	::GetWindowTextW( mWnd, &wideChars[0], numChars + 1 );
	wideChars[numChars] = 0;
	std::string result = msw::toUtf8String( wideChars );
	free( (void*)wideChars );
	return result;
}

void WindowImplMsw::setTitle( const std::string &title )
{
	std::wstring titleWide = msw::toWideString( title );
	if( titleWide.empty() )
		::SetWindowText( mWnd, L"" );
	else
		::SetWindowText( mWnd, titleWide.c_str() );
}

void WindowImplMsw::setSize( const ivec2 &sizePoints )
{
	int screenWidthPx, screenHeightPx;
	ivec2 windowSizePx = toPixels( sizePoints );
	getScreenSize( windowSizePx.x, windowSizePx.y, &screenWidthPx, &screenHeightPx );
	::SetWindowPos( mWnd, HWND_TOP, 0, 0, screenWidthPx, screenHeightPx, SWP_NOMOVE | SWP_NOZORDER );
}

float WindowImplMsw::getContentScale() const
{
	return ( mAppImpl->getHighDensityDisplayEnabled() ) ? mDisplay->getContentScale() : 1.0f;
}

void WindowImplMsw::close()
{
	getAppImpl()->closeWindow( this );
	// at this point 'this' is invalid so do nothing after this line
}

void WindowImplMsw::enableMultiTouch()
{
	// we need to make sure this version of User32 even has MultiTouch symbols, so we'll do that with GetProcAddress
	BOOL (WINAPI *RegisterTouchWindow)( HWND, ULONG);
	*(size_t *)&RegisterTouchWindow = (size_t)::GetProcAddress( ::GetModuleHandle(TEXT("user32.dll")), "RegisterTouchWindow" );
	if( RegisterTouchWindow ) {
		(*RegisterTouchWindow)( mWnd, 0 );
	}
}

void WindowImplMsw::onTouch( HWND hWnd, WPARAM wParam, LPARAM lParam )
{
	// pull these symbols dynamically out of the user32.dll
	static BOOL (WINAPI *GetTouchInputInfo)( HTOUCHINPUT, UINT, PTOUCHINPUT, int ) = nullptr;
	if( ! GetTouchInputInfo )
		*(size_t *)&GetTouchInputInfo = (size_t)::GetProcAddress( ::GetModuleHandle(TEXT("user32.dll")), "GetTouchInputInfo" );
	static BOOL (WINAPI *CloseTouchInputHandle)( HTOUCHINPUT ) = NULL;
	if( ! CloseTouchInputHandle )
		*(size_t *)&CloseTouchInputHandle = (size_t)::GetProcAddress( ::GetModuleHandle(TEXT("user32.dll")), "CloseTouchInputHandle" );

	bool handled = false;
	double currentTime = app::getElapsedSeconds(); // we don't trust the device's sense of time
	unsigned int numInputs = LOWORD( wParam );
	std::unique_ptr<TOUCHINPUT[]> pInputs( new TOUCHINPUT[numInputs] );
	if( pInputs ) {
		vector<TouchEvent::Touch> beganTouches, movedTouches, endTouches, activeTouches;
		if( GetTouchInputInfo((HTOUCHINPUT)lParam, numInputs, pInputs.get(), sizeof(TOUCHINPUT) ) ) {
			for( unsigned int i = 0; i < numInputs; i++ ) {
				const TOUCHINPUT &ti = pInputs.get()[i];
				if( ti.dwID != 0 ) {
					POINT ptI;
					// this has a small problem, which is that we lose the subpixel precision of the touch points.
					// However ScreenToClient doesn't support floating or fixed point either, so we're stuck 
					// unless we write our own ScreenToClient, which actually should be doable
					ptI.x = TOUCH_COORD_TO_PIXEL( ti.x );
					ptI.y = TOUCH_COORD_TO_PIXEL( ti.y );
					::ScreenToClient( hWnd, &ptI );
					vec2 pt( toPoints( (float)ptI.x ), toPoints( (float)ptI.y ) );
					if( ti.dwFlags & 0x0004/*TOUCHEVENTF_UP*/ ) {
						vec2 prevPos = mMultiTouchPrev[ti.dwID];
						endTouches.push_back( TouchEvent::Touch( pt, prevPos, ti.dwID, currentTime, &pInputs.get()[i] ) );
						mMultiTouchPrev.erase( ti.dwID );
					}
					else if( ti.dwFlags & 0x0002/*TOUCHEVENTF_DOWN*/ ) {
						beganTouches.push_back( TouchEvent::Touch( pt, pt, ti.dwID, currentTime, &pInputs.get()[i] ) );
						mMultiTouchPrev[ti.dwID] = pt;
						activeTouches.push_back( beganTouches.back() );
					}
					else if( ti.dwFlags & 0x0001/*TOUCHEVENTF_MOVE*/ ) {
						movedTouches.push_back( TouchEvent::Touch( pt, mMultiTouchPrev[ti.dwID], ti.dwID, currentTime, &pInputs.get()[i] ) );
						activeTouches.push_back( movedTouches.back() );
						mMultiTouchPrev[ti.dwID] = pt;
					}
				}
            }
            
			mActiveTouches = activeTouches;
            
            // we need to post the event here so that our pInputs array is still valid since we've passed addresses into it as the native pointers
            if( ! beganTouches.empty() ) {
				TouchEvent event( getWindow(), beganTouches );
				getWindow()->emitTouchesBegan( &event );
			}
			if( ! movedTouches.empty() ) {
				TouchEvent event( getWindow(), movedTouches );
				getWindow()->emitTouchesMoved( &event );
			}
			if( ! endTouches.empty() ) {
				TouchEvent event( getWindow(), endTouches );
				getWindow()->emitTouchesEnded( &event );
			}
			
            handled = ( ! beganTouches.empty() ) || ( ! movedTouches.empty() ) || ( ! endTouches.empty() );
            CloseTouchInputHandle( (HTOUCHINPUT)lParam ); // this is exception-unsafe; we need some RAII goin' on there*/
        }
        else {
			// for now we'll just ignore an error
        }
	}

    if( ! handled ) {
        // if we didn't handle the message, let DefWindowProc handle it
        ::DefWindowProc( hWnd, WM_TOUCH, wParam, lParam );
    }
}

//! Relative magnification attributed to one detent of Windows' Ctrl+wheel pinch mapping. A precision
//! touchpad reports pinch that way rather than as a true scale, so this is proportional, not measured;
//! 0.1 matches the ~10%-per-detent step Windows apps conventionally use for Ctrl+wheel zoom.
static const float MAGNIFICATION_PER_DETENT = 0.1f;

// The struct is declared locally rather than using the SDK's INPUT_MESSAGE_SOURCE so this builds
// against older Windows SDKs too; the layout (two DWORD-sized enums) is fixed by the API contract.
struct LocalInputMessageSource { DWORD deviceType; DWORD originId; };
typedef BOOL (WINAPI *GetCurrentInputMessageSourceFn)( LocalInputMessageSource* );

//! Resolves GetCurrentInputMessageSource() once. It is Windows 8+, but reporting IMDT_TOUCHPAD as a
//! deviceType requires Windows 10 1703, so it is looked up dynamically and callers need a fallback.
GetCurrentInputMessageSourceFn getInputMessageSourceFn()
{
	static bool sResolved = false;
	static GetCurrentInputMessageSourceFn sFn = nullptr;
	if( ! sResolved ) {
		sResolved = true;
		HMODULE user32 = ::GetModuleHandle( TEXT("user32.dll") );
		if( user32 )
			*(size_t *)&sFn = (size_t)::GetProcAddress( user32, "GetCurrentInputMessageSource" );
	}
	return sFn;
}

//! Returns whether the OS can tell us the device behind a message at all. When false, callers must
//! fall back to guessing, and that guess is unreliable in both directions -- so it is only ever used
//! on systems too old to answer the question properly.
bool isTouchpadDetectionAvailable()
{
	return getInputMessageSourceFn() != nullptr;
}

//! Returns true when the message currently being processed originated from a precision touchpad.
bool isTouchpadInputSource()
{
	GetCurrentInputMessageSourceFn fn = getInputMessageSourceFn();
	if( ! fn )
		return false;

	const DWORD IMDT_TOUCHPAD_VALUE = 0x10; // INPUT_MESSAGE_DEVICE_TYPE::IMDT_TOUCHPAD, Windows 10 1703+
	LocalInputMessageSource source = { 0, 0 };
	if( ! (*fn)( &source ) )
		return false;

	return source.deviceType == IMDT_TOUCHPAD_VALUE;
}

unsigned int prepMouseEventModifiers( WPARAM wParam )
{
	unsigned int result = 0;
	if( wParam & MK_CONTROL ) result |= MouseEvent::CTRL_DOWN;
	if( wParam & MK_LBUTTON ) result |= MouseEvent::LEFT_DOWN;
	if( wParam & MK_MBUTTON ) result |= MouseEvent::MIDDLE_DOWN;
	if( wParam & MK_RBUTTON ) result |= MouseEvent::RIGHT_DOWN;
	if( wParam & MK_SHIFT ) result |= MouseEvent::SHIFT_DOWN;
	if( ::GetKeyState( VK_MENU ) < 0 ) result |= MouseEvent::ALT_DOWN;	
	if( (::GetKeyState( VK_LWIN ) < 0) || (::GetKeyState( VK_RWIN ) < 0) ) result |= MouseEvent::META_DOWN;
	return result;
}

// Certain key codes need to be refined, for example VK_MENU needs to be
// converted into VK_LALT or VK_RALT
int prepNativeKeyCode( WPARAM wParam )
{
	unsigned int result = (int)wParam;
	if( wParam == VK_MENU ) {
		result = ( ::GetKeyState( VK_RMENU ) ) ? VK_RMENU : VK_LMENU;
	}
	else if( wParam == VK_SHIFT ) {
		result = ( ::GetKeyState( VK_RSHIFT ) ) ? VK_RSHIFT : VK_LSHIFT;	
	}
	else if( wParam == VK_CONTROL ) {
		result = ( ::GetKeyState( VK_RCONTROL ) ) ? VK_RCONTROL : VK_LCONTROL;		
	}
	return result;
}

WCHAR mapVirtualKey( WPARAM wParam )
{
	BYTE keyboardState[256];
	::GetKeyboardState( keyboardState );
	WCHAR result[4] = { 0, 0, 0, 0 };

	// the control key messes up the ToAscii result, so we zero it out
	keyboardState[VK_CONTROL] = 0;

	int resultLength = ::ToUnicode( static_cast<UINT>( wParam ), ::MapVirtualKey( static_cast<UINT>( wParam ), 0 ), keyboardState, result, 4, 0 );
	if( resultLength >= 1 )
		return result[0];
	else
		return 0;
}

unsigned int prepKeyEventModifiers()
{
	unsigned int result = 0;
	if( ::GetKeyState( VK_CONTROL ) & 0x8000 ) result |= KeyEvent::CTRL_DOWN;
	if( ::GetKeyState( VK_SHIFT ) & 0x8000 ) result |= KeyEvent::SHIFT_DOWN;
	if( ( ::GetKeyState( VK_LMENU ) & 0x8000 ) || ( ::GetKeyState( VK_RMENU ) & 0x8000 ) ) result |= KeyEvent::ALT_DOWN;	
	if( ( ::GetKeyState( VK_LWIN ) < 0 ) || ( ::GetKeyState( VK_RWIN ) < 0 ) ) result |= KeyEvent::META_DOWN;
	return result;
}

void WindowImplMsw::setBorderless( bool borderless )
{
	if( mBorderless != borderless ) {
		mBorderless = borderless;
		if( mBorderless ) {
			mWindowExStyle = WS_EX_APPWINDOW;
			mWindowStyle = WS_POPUP;
		}
		else {
			mWindowExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_ACCEPTFILES;	// Window Extended Style
			mWindowStyle = ( mResizable ) ? WS_OVERLAPPEDWINDOW
				:	( WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME );							// Windows Style
		}

		POINT upperLeft;
		upperLeft.x = upperLeft.y = 0;
		::ClientToScreen( mWnd, &upperLeft );

		RECT windowRect;
		::GetClientRect( mWnd, &windowRect );
		windowRect.left += upperLeft.x; windowRect.right += upperLeft.x;
		windowRect.top += upperLeft.y; windowRect.bottom += upperLeft.y;
		::AdjustWindowRectEx( &windowRect, mWindowStyle, FALSE, mWindowExStyle );		// Adjust Window To True Requested Size

		::SetWindowLongA( mWnd, GWL_STYLE, mWindowStyle );
		::SetWindowLongA( mWnd, GWL_EXSTYLE, mWindowExStyle );
		::SetWindowPos( mWnd, HWND_TOP, windowRect.left, windowRect.top, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
				SWP_FRAMECHANGED|SWP_SHOWWINDOW|SWP_NOZORDER );
		if( mBorderless )
			::InvalidateRect( 0, NULL, TRUE );
	}
}

void WindowImplMsw::setAlwaysOnTop( bool alwaysOnTop )
{
	if( mAlwaysOnTop != alwaysOnTop ) {
		mAlwaysOnTop = alwaysOnTop;
		LONG oldExStyle = ::GetWindowLongA( mWnd, GWL_EXSTYLE );
		if( mAlwaysOnTop ) {
			::SetWindowLongA( mWnd, GWL_EXSTYLE, oldExStyle | WS_EX_TOPMOST );
			::SetWindowPos( mWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE );
		}
		else {
			::SetWindowLongA( mWnd, GWL_EXSTYLE, oldExStyle &= (~WS_EX_TOPMOST) );
			::SetWindowPos( mWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE );
		}
	}
}

LRESULT CALLBACK WndProc(	HWND	mWnd,			// Handle For This Window
							UINT	uMsg,			// Message For This Window
							WPARAM	wParam,			// Additional Message Information
							LPARAM	lParam)			// Additional Message Information
{
	WindowImplMsw* impl;

	// if the message is WM_NCCREATE we need to hide 'this' in the window long
	if( uMsg == WM_NCCREATE ) {
		impl = reinterpret_cast<WindowImplMsw*>(((LPCREATESTRUCT)lParam)->lpCreateParams);
		::SetWindowLongPtr( mWnd, GWLP_USERDATA, (__int3264)(LONG_PTR)impl ); 
	}
	else // the warning on this line is harmless
		impl = reinterpret_cast<WindowImplMsw*>( ::GetWindowLongPtr( mWnd, GWLP_USERDATA ) );

	if( uMsg == WM_DESTROY )
		detachDirectManipulation( mWnd );

	if( ! impl )
		return DefWindowProc( mWnd, uMsg, wParam, lParam );		
	impl->mAppImpl->setWindow( impl->mWindowRef );

	switch( uMsg ) {
		case WM_SYSCOMMAND:
			switch( wParam ) {
				case SC_SCREENSAVE:					// Screensaver Trying To Start?
				case SC_MONITORPOWER:				// Monitor Trying To Enter Powersave?
					if( ! impl->getAppImpl()->getApp()->isPowerManagementEnabled() )
						return false;
					else
						return DefWindowProc( mWnd, uMsg, wParam, lParam );
				case SC_KEYMENU:				    // Is native menu invoked?
					if (HISHORT(lParam) <= 0)		// Is it invoked by keyboard?
						return false;
					else
						return DefWindowProc(mWnd, uMsg, wParam, lParam);
					break;
			}
		break;
		case WM_ACTIVATEAPP:
			if( wParam ) {
				if( ! impl->getAppImpl()->mActive ) {
					impl->getAppImpl()->mActive = true;
					impl->getAppImpl()->getApp()->emitDidBecomeActive();
				}
			}
			else {
				if( impl->getAppImpl()->mActive ) {
					impl->getAppImpl()->mActive = false;
					impl->getAppImpl()->getApp()->emitWillResignActive();
				}
			}
		break;
		case WM_ACTIVATE:
			if( ( wParam == WA_ACTIVE ) || ( wParam == WA_CLICKACTIVE ) )
				impl->getAppImpl()->setForegroundWindow( impl->getWindow() );
		break;
		case WM_CLOSE:								// Did We Receive A Close Message?
			impl->getAppImpl()->closeWindow( impl );
			// be careful not to do anything with 'impl' after here
			return 0;
		break;
		case WM_SYSKEYDOWN:
		case WM_KEYDOWN: {
			WCHAR c = mapVirtualKey( wParam );
			KeyEvent event( impl->getWindow(), KeyEvent::translateNativeKeyCode( prepNativeKeyCode( wParam ) ),
							c, static_cast<char>( c ), prepKeyEventModifiers(), static_cast<unsigned int>(wParam) );
			impl->getWindow()->emitKeyDown( &event );
			if ( event.isHandled() )
				return 0;
		}
		break;
		case WM_SYSKEYUP:
		case WM_KEYUP: {
			WCHAR c = mapVirtualKey( wParam );
			KeyEvent event( impl->getWindow(), KeyEvent::translateNativeKeyCode( prepNativeKeyCode( wParam ) ),
							c, static_cast<char>( c ), prepKeyEventModifiers(), static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitKeyUp( &event );
			if ( event.isHandled() )
				return 0;
		}
		break;
		// mouse events
		case WM_LBUTTONDOWN: {
			::SetCapture( mWnd );
			impl->mIsDragging = true;
			MouseEvent event( impl->getWindow(), MouseEvent::LEFT_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseDown( &event );
			return 0;
		}
		break;
		case WM_RBUTTONDOWN: {
			::SetCapture( mWnd );
			impl->mIsDragging = true;
			MouseEvent event( impl->getWindow(), MouseEvent::RIGHT_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseDown( &event );
			return 0;
		}
		break;		
		case WM_MBUTTONDOWN: {
			::SetCapture( mWnd );
			impl->mIsDragging = true;		
			MouseEvent event( impl->getWindow(), MouseEvent::MIDDLE_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseDown( &event );
			return 0;
		}
		break;
		case WM_LBUTTONUP: {
			::ReleaseCapture();
			impl->mIsDragging = false;
			MouseEvent event( impl->getWindow(), MouseEvent::LEFT_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseUp( &event );
			return 0;
		}
		break;
		case WM_RBUTTONUP: {
			::ReleaseCapture();
			impl->mIsDragging = false;		
			MouseEvent event( impl->getWindow(), MouseEvent::RIGHT_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseUp( &event );
			return 0;
		}
		break;		
		case WM_MBUTTONUP: {
			::ReleaseCapture();
			impl->mIsDragging = false;
			MouseEvent event( impl->getWindow(), MouseEvent::MIDDLE_DOWN, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
			impl->getWindow()->emitMouseUp( &event );
			return 0;
		}
		break;
		case DM_POINTERHITTEST_MSG:
			if( directManipulationHitTest( mWnd, wParam ) )
				return 0;
		break;
		case WM_TIMER:
			if( wParam == DM_UPDATE_TIMER_ID ) {
				float scale;
				vec2 pan;
				directManipulationUpdate( mWnd, &scale, &pan );
				if( scale == 1.0f && pan == vec2( 0 ) )
					return 0;

				POINT pt;
				::GetCursorPos( &pt );
				::ScreenToClient( mWnd, &pt );
				const unsigned int mods = currentMouseModifiers();
				if( scale != 1.0f ) {
					MouseEvent event( impl->getWindow(), 0, impl->toPoints((int)pt.x), impl->toPoints((int)pt.y), mods, 0.0f, 0 );
					event.setMagnification( scale - 1.0f );
					impl->getWindow()->emitGestureMagnify( &event );
				}
				if( pan != vec2( 0 ) ) {
					// Same convention as WM_MOUSEWHEEL/WM_MOUSEHWHEEL below: content moving down scrolls up (+Y),
					// content moving right scrolls left (+X).
					const vec2 delta = pan / impl->getContentScale() / MouseEvent::PRECISE_PIXELS_PER_DETENT;
					MouseEvent event( impl->getWindow(), 0, impl->toPoints((int)pt.x), impl->toPoints((int)pt.y), mods, delta.y, 0 );
					event.setScrollDelta( delta, true );
					impl->getWindow()->emitMouseWheel( &event );
				}
				return 0;
			}
		break;
		case WM_MOUSEWHEEL: {
			POINT pt = { ((int)(short)LOWORD(lParam)), ((int)(short)HIWORD(lParam)) };
			::MapWindowPoints( NULL, mWnd, &pt, 1 );
			const float delta = GET_WHEEL_DELTA_WPARAM( wParam ) / 120.0f;
			const unsigned int mods = prepMouseEventModifiers( wParam );

			// Ask the OS what device sent this rather than guessing from the magnitude. The old heuristic
			// ("a real wheel never sends less than one detent") was wrong both ways: a touchpad scrolling
			// fast exceeds a detent per message, and high-resolution wheel mice routinely send less.
			const bool touchpad = isTouchpadInputSource();
			observeTouchpadWheel( touchpad );
			const bool precise = touchpad || ( ! isTouchpadDetectionAvailable() && delta != 0.0f && fabsf( delta ) < 1.0f );

			// A precision touchpad maps pinch-to-zoom onto Ctrl+wheel for legacy apps. Knowing the device
			// lets us tell that apart from someone genuinely Ctrl+scrolling a mouse, and route it to the
			// magnify channel so it never reaches wheel handlers as a scroll.
			if( touchpad && ( mods & MouseEvent::CTRL_DOWN ) ) {
				MouseEvent event( impl->getWindow(), 0, impl->toPoints((int)pt.x), impl->toPoints((int)pt.y),
									mods & ~MouseEvent::CTRL_DOWN, 0.0f, static_cast<unsigned int>( wParam ) );
				event.setMagnification( delta * MAGNIFICATION_PER_DETENT );
				impl->getWindow()->emitGestureMagnify( &event );
				break;
			}

			MouseEvent event( impl->getWindow(), 0, impl->toPoints((int)pt.x), impl->toPoints((int)pt.y), mods,
								delta, static_cast<unsigned int>( wParam ) );
			event.setScrollDelta( vec2( 0, delta ), precise );
			impl->getWindow()->emitMouseWheel( &event );
		}
		break;
		case WM_MOUSEHWHEEL: {
			POINT pt = { ((int)(short)LOWORD(lParam)), ((int)(short)HIWORD(lParam)) };
			::MapWindowPoints( NULL, mWnd, &pt, 1 );
			// Positive is scroll-right on Windows; negate so +X matches the macOS/`getWheelDelta()` convention.
			const float delta = -( GET_WHEEL_DELTA_WPARAM( wParam ) / 120.0f );
			const bool touchpad = isTouchpadInputSource();
			observeTouchpadWheel( touchpad );
			const bool precise = touchpad ||
					( ! isTouchpadDetectionAvailable() && delta != 0.0f && fabsf( delta ) < 1.0f );
			// Legacy scalar stays 0: existing single-axis handlers must not react to horizontal scroll.
			MouseEvent event( impl->getWindow(), 0, impl->toPoints((int)pt.x), impl->toPoints((int)pt.y), prepMouseEventModifiers( wParam ),
								0.0f, static_cast<unsigned int>( wParam ) );
			event.setScrollDelta( vec2( delta, 0 ), precise );
			impl->getWindow()->emitMouseWheel( &event );
		}
		break;
		case WM_KILLFOCUS:
			// if we lose capture during a drag, post a mouseup event as a notifier
			if( impl->mIsDragging ) {
				MouseEvent event( impl->getWindow(), 0, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
				impl->getWindow()->emitMouseUp( &event );
			}
			impl->mIsDragging = false;
		break;
		case WM_MOUSEMOVE: {
			if( impl->mIsDragging ) {
				MouseEvent event( impl->getWindow(), 0, impl->toPoints( LOSHORT(lParam) ), impl->toPoints( HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
				impl->getWindow()->emitMouseDrag( &event );						
			}
			else {
				MouseEvent event( impl->getWindow(), 0, impl->toPoints( LOSHORT(lParam) ), impl->toPoints(  HISHORT(lParam) ), prepMouseEventModifiers( wParam ), 0.0f, static_cast<unsigned int>( wParam ) );
				impl->getWindow()->emitMouseMove( &event );
			}
		}
		break;
		case WM_SIZE:
			impl->mWindowWidthPx = LOWORD(lParam);
			impl->mWindowHeightPx = HIWORD(lParam);
			if( impl->getWindow() && impl->mAppImpl->setupHasBeenCalled() ) {
				impl->getWindow()->emitResize();
			}
			return 0;
		break;
		case WM_MOVE: {
			impl->mWindowOffset = ivec2( LOSHORT(lParam), HISHORT(lParam) );
			if( impl->getWindow() ) {
				DisplayRef oldDisplay = impl->mDisplay;
				impl->mDisplay = PlatformMsw::get()->findDisplayFromHmonitor( ::MonitorFromWindow( mWnd, MONITOR_DEFAULTTONEAREST ) );
				// signal display change as appropriate
				if( oldDisplay != impl->mDisplay ) {
					impl->getWindow()->emitDisplayChange();
				}
				// signal window move
				impl->getWindow()->emitMove();
			}
			return 0;
		}
		break;
		case WM_DROPFILES: {
			HDROP dropH = (HDROP)wParam;
			POINT dropPoint;
			wchar_t fileName[8192];
			vector<fs::path> files;
			
			int droppedFileCount = ::DragQueryFile( dropH, 0xFFFFFFFF, 0, 0 );
			for( int i = 0; i < droppedFileCount; ++i ) {
				::DragQueryFileW( dropH, i, fileName, 8192 );
				files.push_back( fileName );
			}

			::DragQueryPoint( dropH, &dropPoint );
			::DragFinish( dropH );

			FileDropEvent dropEvent( impl->getWindow(), impl->toPoints( (int)dropPoint.x ), impl->toPoints( (int)dropPoint.y ), files );
			impl->getWindow()->emitFileDrop( &dropEvent );
			return 0;
		}
		break;
		case WM_PAINT:
			if( impl->getAppImpl()->setupHasBeenCalled() )
				impl->draw();
		break;
		case WM_TOUCH:
			impl->onTouch( mWnd, wParam, lParam );
		break;
		case WM_DISPLAYCHANGE:
			impl->mAppImpl->mNeedsToRefreshDisplays = true;
		break;
		case WM_DEVICECHANGE:
			impl->mAppImpl->mNeedsToRefreshDisplays = true;
		break;
		case 0x02E0 /*WM_DPICHANGED*/:
			if( impl->mAppImpl->setupHasBeenCalled() ) {
				impl->mAppImpl->mNeedsToRefreshDisplays = true;
			}
		break;
	}

	// unhandled messages To DefWindowProc
	return DefWindowProc( mWnd, uMsg, wParam, lParam );
}

void WindowImplMsw::draw()
{
	mAppImpl->setWindow( mWindowRef );
	mRenderer->startDraw();
	mWindowRef->emitDraw();
	mRenderer->finishDraw();
}

void WindowImplMsw::resize()
{
	mAppImpl->setWindow( mWindowRef );
	mWindowRef->emitResize();
}

void WindowImplMsw::redraw()
{
	::RedrawWindow( mWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
}

void WindowImplMsw::privateClose()
{
	mRenderer->kill();

	if( mDC )
		::ReleaseDC( mWnd, mDC );

	if( mWnd )
		::DestroyWindow( mWnd );

	mWnd = 0;
}

void WindowImplMsw::keyDown( const KeyEvent &event )
{
	KeyEvent localEvent( event );
	mAppImpl->setWindow( mWindowRef );
	mWindowRef->emitKeyDown( &localEvent );
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BlankingWindow
BlankingWindow::BlankingWindow( DisplayRef display )
{
	registerWindowClass();

	RECT windowRect;
	Area displayArea = display->getBounds();
	windowRect.left = displayArea.getX1();
	windowRect.right = displayArea.getX2();
	windowRect.top = displayArea.getY1();
	windowRect.bottom = displayArea.getY2();

	UINT windowExStyle = WS_EX_APPWINDOW;
	UINT windowStyle = WS_POPUP;

	::AdjustWindowRectEx( &windowRect, windowStyle, FALSE, windowExStyle );		// Adjust Window To True Requested Size

	std::wstring unicodeTitle = L"";

	// Create The Window
	if( 0 == (( mWnd = ::CreateWindowEx( windowExStyle,						// Extended Style For The Window
		BLANKING_WINDOW_CLASS_NAME,
		unicodeTitle.c_str(),
		windowStyle,					// Required Window Style
		windowRect.left, windowRect.top,								// Window Position
		windowRect.right - windowRect.left,	// Calculate Window Width
		windowRect.bottom - windowRect.top,	// Calculate Window Height
		NULL,								// No Parent Window
		NULL,								// No Menu
		::GetModuleHandle( NULL ),
		reinterpret_cast<LPVOID>( this ) ) )) )
	{
		//killWindow();							// Reset The Display
		return;
	}

	::ShowWindow( mWnd, SW_SHOW );
	::SetWindowLongA( mWnd, GWL_EXSTYLE, ::GetWindowLongA( mWnd, GWL_EXSTYLE ) | WS_EX_TOPMOST );
	::SetWindowPos( mWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE );
	::SetForegroundWindow( mWnd );
	::SetFocus( mWnd );

}

void BlankingWindow::registerWindowClass()
{
	static bool sRegistered = false;

	if( sRegistered )
		return;

	WNDCLASS	wc;
	HMODULE instance	= ::GetModuleHandle( NULL );				// Grab An Instance For Our Window
	wc.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;	// Redraw On Size, And Own DC For Window.
	wc.lpfnWndProc		= BlankingWndProc;						// WndProc Handles Messages
	wc.cbClsExtra		= 0;									// No Extra Window Data
	wc.cbWndExtra		= 0;									// No Extra Window Data
	wc.hInstance		= instance;							// Set The Instance
	wc.hIcon			= ::LoadIcon( NULL, IDI_WINLOGO );		// Load The Default Icon
	wc.hCursor			= ::LoadCursor( NULL, IDC_ARROW );		// Load The Arrow Pointer
	wc.hbrBackground	= NULL;									// No Background Required For GL
	wc.lpszMenuName		= NULL;									// We Don't Want A Menu
	wc.lpszClassName	= BLANKING_WINDOW_CLASS_NAME;

	if( ! ::RegisterClass( &wc ) ) {								// Attempt To Register The Window Class
		DWORD err = ::GetLastError();
		return;							
	}

	sRegistered = true;
}

LRESULT CALLBACK BlankingWndProc( HWND wnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	static HBRUSH backgroundBrush = ::CreateSolidBrush( RGB( 0, 0, 0 ) );
	
	switch( uMsg ) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			RECT clientRect;
			HDC hdc = ::BeginPaint( wnd, &ps );
			::GetClientRect( wnd, &clientRect );
			::SelectObject( hdc, backgroundBrush );
			::Rectangle( hdc, 0, 0, clientRect.right, clientRect.bottom );
			::EndPaint( wnd, &ps );
			return 0;
		}
		break;
		default:
			return ::DefWindowProc( wnd, uMsg, wParam, lParam );
	}
}

void BlankingWindow::destroy()
{
	if( mWnd )
		::DestroyWindow( mWnd );
}

} } // namespace cinder::app
