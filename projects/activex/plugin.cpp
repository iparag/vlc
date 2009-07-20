/*****************************************************************************
 * plugin.cpp: ActiveX control for VLC
 *****************************************************************************
 * Copyright (C) 2006 the VideoLAN team
 *
 * Authors: Damien Fouilleul <Damien.Fouilleul@laposte.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "plugin.h"

#include "oleobject.h"
#include "olecontrol.h"
#include "oleinplaceobject.h"
#include "oleinplaceactiveobject.h"
#include "persistpropbag.h"
#include "persiststreaminit.h"
#include "persiststorage.h"
#include "provideclassinfo.h"
#include "connectioncontainer.h"
#include "objectsafety.h"
#include "vlccontrol.h"
#include "vlccontrol2.h"
#include "viewobject.h"
#include "dataobject.h"
#include "supporterrorinfo.h"

#include "utils.h"

#include <string.h>
#include <winreg.h>
#include <winuser.h>
#include <servprov.h>
#include <shlwapi.h>
#include <wininet.h>

using namespace std;

extern "C" void f_write_log(char*,...);
#define WM_VLC_TRANSLATE_MESSAGE WM_USER+0x100
#define _D_SYNCHRONIZE
////////////////////////////////////////////////////////////////////////
//class factory

static LRESULT CALLBACK VLCInPlaceClassWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    VLCPlugin *p_instance = reinterpret_cast<VLCPlugin *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch( uMsg )
    {
        case WM_DESTROY:
         if(p_instance) p_instance->onDestroy();
        case WM_ERASEBKGND:
            return 1L;

        case WM_PAINT:
            PAINTSTRUCT ps;
            RECT pr;
            if( GetUpdateRect(hWnd, &pr, FALSE) )
            {
                RECT bounds;
                GetClientRect(hWnd, &bounds);
                BeginPaint(hWnd, &ps);
                p_instance->onPaint(ps.hdc, bounds, pr);
                EndPaint(hWnd, &ps);
            }
            return 0L;

        case WM_MOUSEMOVE:
         if( p_instance && p_instance->isUserMode() ) 
          p_instance->fireMouseMove(LOWORD(lParam),HIWORD(lParam));
         break;
        case WM_NCMOUSEMOVE:
         if( p_instance && p_instance->isUserMode() ) 
          p_instance->fireNCMouseMove(LOWORD(lParam),HIWORD(lParam));
         break;
        case WM_VLC_TRANSLATE_MESSAGE:
          if(p_instance) p_instance->onTimer();
          return 0L;
        default:
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
};

VLCPluginClass::VLCPluginClass(LONG *p_class_ref, HINSTANCE hInstance, REFCLSID rclsid) :
    _p_class_ref(p_class_ref),
    _hinstance(hInstance),
    _classid(rclsid),
    _inplace_picture(NULL)
{
    WNDCLASS wClass;

    if( ! GetClassInfo(hInstance, getInPlaceWndClassName(), &wClass) )
    {
        wClass.style          = CS_NOCLOSE|CS_DBLCLKS;
        wClass.lpfnWndProc    = VLCInPlaceClassWndProc;
        wClass.cbClsExtra     = 0;
        wClass.cbWndExtra     = 0;
        wClass.hInstance      = hInstance;
        wClass.hIcon          = NULL;
        wClass.hCursor        = LoadCursor(NULL, IDC_ARROW);
        wClass.hbrBackground  = NULL;
        wClass.lpszMenuName   = NULL;
        wClass.lpszClassName  = getInPlaceWndClassName();

        _inplace_wndclass_atom = RegisterClass(&wClass);
    }
    else
    {
        _inplace_wndclass_atom = 0;
    }

    HBITMAP hbitmap = (HBITMAP)LoadImage(getHInstance(), MAKEINTRESOURCE(2), IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
    if( NULL != hbitmap )
    {
        PICTDESC pictDesc;

        pictDesc.cbSizeofstruct = sizeof(PICTDESC);
        pictDesc.picType        = PICTYPE_BITMAP;
        pictDesc.bmp.hbitmap    = hbitmap;
        pictDesc.bmp.hpal       = NULL;

        if( FAILED(OleCreatePictureIndirect(&pictDesc, IID_IPicture, TRUE, reinterpret_cast<LPVOID*>(&_inplace_picture))) )
            _inplace_picture = NULL;
    }
    AddRef();
};

VLCPluginClass::~VLCPluginClass()
{
    if( 0 != _inplace_wndclass_atom )
        UnregisterClass(MAKEINTATOM(_inplace_wndclass_atom), _hinstance);

    if( NULL != _inplace_picture )
        _inplace_picture->Release();
};

STDMETHODIMP VLCPluginClass::QueryInterface(REFIID riid, void **ppv)
{
    if( NULL == ppv )
        return E_INVALIDARG;

    if( (IID_IUnknown == riid)
     || (IID_IClassFactory == riid) )
    {
        AddRef();
        *ppv = reinterpret_cast<LPVOID>(this);

        return NOERROR;
    }

    *ppv = NULL;

    return E_NOINTERFACE;
};

STDMETHODIMP_(ULONG) VLCPluginClass::AddRef(void)
{
    return InterlockedIncrement(_p_class_ref);
};

STDMETHODIMP_(ULONG) VLCPluginClass::Release(void)
{
    ULONG refcount = InterlockedDecrement(_p_class_ref);
    if( 0 == refcount )
    {
        delete this;
        return 0;
    }
    return refcount;
};

STDMETHODIMP VLCPluginClass::CreateInstance(LPUNKNOWN pUnkOuter, REFIID riid, void **ppv)
{
    if( NULL == ppv )
        return E_POINTER;

    *ppv = NULL;

    if( (NULL != pUnkOuter) && (IID_IUnknown != riid) ) {
        return CLASS_E_NOAGGREGATION;
    }

    VLCPlugin *plugin = new VLCPlugin(this, pUnkOuter);
    if( NULL != plugin )
    {
        HRESULT hr = plugin->QueryInterface(riid, ppv);
        // the following will destroy the object if QueryInterface() failed
        plugin->Release();
        return hr;
    }
    return E_OUTOFMEMORY;
};

STDMETHODIMP VLCPluginClass::LockServer(BOOL fLock)
{
    if( fLock )
        AddRef();
    else
        Release();

    return S_OK;
};

////////////////////////////////////////////////////////////////////////

VLCPlugin::VLCPlugin(VLCPluginClass *p_class, LPUNKNOWN pUnkOuter) :
    _inplacewnd(NULL),
    _p_class(p_class),
    _i_ref(1UL),
    _p_libvlc(NULL),
    _i_codepage(CP_ACP),
    _b_usermode(TRUE)
{
    p_class->AddRef();

    vlcOleControl = new VLCOleControl(this);
    vlcOleInPlaceObject = new VLCOleInPlaceObject(this);
    vlcOleInPlaceActiveObject = new VLCOleInPlaceActiveObject(this);
    vlcPersistStorage = new VLCPersistStorage(this);
    vlcPersistStreamInit = new VLCPersistStreamInit(this);
    vlcPersistPropertyBag = new VLCPersistPropertyBag(this);
    vlcProvideClassInfo = new VLCProvideClassInfo(this);
    vlcConnectionPointContainer = new VLCConnectionPointContainer(this);
    vlcObjectSafety = new VLCObjectSafety(this);
    vlcControl = new VLCControl(this);
    vlcControl2 = new VLCControl2(this);
    vlcViewObject = new VLCViewObject(this);
    vlcDataObject = new VLCDataObject(this);
    vlcOleObject = new VLCOleObject(this);
    vlcSupportErrorInfo = new VLCSupportErrorInfo(this);

    // configure controlling IUnknown interface for implemented interfaces
    this->pUnkOuter = (NULL != pUnkOuter) ? pUnkOuter : dynamic_cast<LPUNKNOWN>(this);

    // default picure
    _p_pict = NULL;
    
    _paused_bitmap = NULL;
    _paused_bitmap_size.cx = 0;
    _paused_bitmap_size.cy = 0;
    _render_bitmap = NULL;
    _render_bitmap_size.cx = 0;
    _render_bitmap_size.cy = 0;

    // make sure that persistable properties are initialized
    onInit();
};

VLCPlugin::~VLCPlugin()
{
    /*
    ** bump refcount to avoid recursive release from
    ** following interfaces when releasing this interface
    */
    AddRef();

    delete vlcSupportErrorInfo;
    delete vlcOleObject;
    delete vlcDataObject;
    delete vlcViewObject;
    delete vlcControl2;
    delete vlcControl;
    delete vlcConnectionPointContainer;
    delete vlcProvideClassInfo;
    delete vlcPersistPropertyBag;
    delete vlcPersistStreamInit;
    delete vlcPersistStorage;
    delete vlcOleInPlaceActiveObject;
    delete vlcOleInPlaceObject;
    delete vlcObjectSafety;

    delete vlcOleControl;
    if( _p_pict )
        _p_pict->Release();

    SysFreeString(_bstr_mrl);
    SysFreeString(_bstr_baseurl);

    _p_class->Release();
   
    if( _paused_bitmap ) DeleteObject( _paused_bitmap );
    if( _render_bitmap ) DeleteObject( _render_bitmap );
};

STDMETHODIMP VLCPlugin::QueryInterface(REFIID riid, void **ppv)
{
    if( NULL == ppv )
        return E_INVALIDARG;

    if( IID_IUnknown == riid )
        *ppv = reinterpret_cast<LPVOID>(this);
    else if( IID_IOleObject == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcOleObject);
    else if( IID_IOleControl == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcOleControl);
    else if( IID_IOleWindow == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcOleInPlaceObject);
    else if( IID_IOleInPlaceObject == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcOleInPlaceObject);
    else if( IID_IOleInPlaceActiveObject == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcOleInPlaceActiveObject);
    else if( IID_IPersist == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcPersistStreamInit);
    else if( IID_IPersistStreamInit == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcPersistStreamInit);
    else if( IID_IPersistStorage == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcPersistStorage);
    else if( IID_IPersistPropertyBag == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcPersistPropertyBag);
    else if( IID_IProvideClassInfo == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcProvideClassInfo);
    else if( IID_IProvideClassInfo2 == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcProvideClassInfo);
    else if( IID_IConnectionPointContainer == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcConnectionPointContainer);
    else if( IID_IObjectSafety == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcObjectSafety);
    else if( IID_IDispatch == riid )
        *ppv = (CLSID_VLCPlugin2 == getClassID()) ?
                reinterpret_cast<LPVOID>(vlcControl2) :
                reinterpret_cast<LPVOID>(vlcControl);
    else if( IID_IVLCControl == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcControl);
    else if( IID_IVLCControl2 == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcControl2);
    else if( IID_IViewObject == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcViewObject);
    else if( IID_IViewObject2 == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcViewObject);
    else if( IID_IDataObject == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcDataObject);
    else if( IID_ISupportErrorInfo == riid )
        *ppv = reinterpret_cast<LPVOID>(vlcSupportErrorInfo);
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ((LPUNKNOWN)*ppv)->AddRef();
    return NOERROR;
};

STDMETHODIMP_(ULONG) VLCPlugin::AddRef(void)
{
    return InterlockedIncrement((LONG *)&_i_ref);
};

STDMETHODIMP_(ULONG) VLCPlugin::Release(void)
{
    if( ! InterlockedDecrement((LONG *)&_i_ref) )
    {
        delete this;
        return 0;
    }
    return _i_ref;
};

//////////////////////////////////////

HRESULT VLCPlugin::onInit(void)
{
    if( NULL == _p_libvlc )
    {
        // initialize persistable properties
        _b_autoplay   = TRUE;
        _b_autoloop   = FALSE;
        _b_toolbar    = FALSE;
        _bstr_baseurl = NULL;
        _bstr_mrl     = NULL;
        _b_visible    = TRUE;
        _b_mute       = FALSE;
        _i_volume     = 50;
        _i_time       = 0;
        _i_backcolor  = 0;
        // set default/preferred size (320x240) pixels in HIMETRIC
        HDC hDC = CreateDevDC(NULL);
        _extent.cx = 320;
        _extent.cy = 240;
        HimetricFromDP(hDC, (LPPOINT)&_extent, 1);
        DeleteDC(hDC);

        return S_OK;
    }
    return CO_E_ALREADYINITIALIZED;
};

void VLCPlugin::pushMessage( _s_message_t message )
{
 if( !isInPlaceActive() ) return;
 _s_message_t *p_message=new _s_message_t();
 *p_message = message;
 _q_events.push(p_message); 
 PostMessage( _inplacewnd, WM_VLC_TRANSLATE_MESSAGE, 0, 0L );
}

_s_message_t VLCPlugin::popMessage()
{
 _s_message_t message,*p_message;

 message._i_type = (vlc_event_type_t)0;
 if( !_q_events.empty() )
 {
  p_message = _q_events.front();
  message = *p_message;
  _q_events.pop();
  delete p_message;
 }
 return message;
}

BOOL VLCPlugin::findMessage( _s_message_t message )
{
 return FALSE;
}

HRESULT VLCPlugin::onLoad(void)
{
    if( SysStringLen(_bstr_baseurl) == 0 )
    {
        /*
        ** try to retreive the base URL using the client site moniker, which for Internet Explorer
        ** is the URL of the page the plugin is embedded into.
        */
        LPOLECLIENTSITE pClientSite;
        if( SUCCEEDED(vlcOleObject->GetClientSite(&pClientSite)) && (NULL != pClientSite) )
        {
            IBindCtx *pBC = 0;
            if( SUCCEEDED(CreateBindCtx(0, &pBC)) )
            {
                LPMONIKER pContMoniker = NULL;
                if( SUCCEEDED(pClientSite->GetMoniker(OLEGETMONIKER_ONLYIFTHERE,
                                OLEWHICHMK_CONTAINER, &pContMoniker)) )
                {
                    LPOLESTR base_url;
                    if( SUCCEEDED(pContMoniker->GetDisplayName(pBC, NULL, &base_url)) )
                    {
                        /*
                        ** check that the moniker name is a URL
                        */
                        if( UrlIsW(base_url, URLIS_URL) )
                        {
                            /* copy base URL */
                            _bstr_baseurl = SysAllocString(base_url);
                        }
                        CoTaskMemFree(base_url);
                    }
                }
            }
        }
    }
    setDirty(FALSE);
    return S_OK;
};

extern "C" void f_write_log(char*,...);
struct s_event_t
{
 vlc_event_type_t _i_event;
 vlc_event_callback_t _pf_callback;
};

s_event_t p_event_callbacks[]=
{
 { vlc_InputThreadFinished, (vlc_event_callback_t)VLCPlugin::onInputThreadFinished },
 { vlc_OutputThreadStarted, (vlc_event_callback_t)VLCPlugin::onOutputThreadStarted },
 { vlc_InputThreadStopResponding, (vlc_event_callback_t)VLCPlugin::onInputThreadStopResponding },
 { vlc_InputThreadResumeResponding, (vlc_event_callback_t)VLCPlugin::onInputThreadResumeResponding },
 { vlc_MouseMove, (vlc_event_callback_t)VLCPlugin::onMouseMove },
 { vlc_NCMouseMove, (vlc_event_callback_t)VLCPlugin::onNCMouseMove },
 { vlc_LButtonDown, (vlc_event_callback_t)VLCPlugin::onLButtonDown },
 { vlc_LButtonUp, (vlc_event_callback_t)VLCPlugin::onLButtonUp },
 { vlc_MButtonDown, (vlc_event_callback_t)VLCPlugin::onMButtonDown },
 { vlc_MButtonUp, (vlc_event_callback_t)VLCPlugin::onMButtonUp },
 { vlc_RButtonDown, (vlc_event_callback_t)VLCPlugin::onRButtonDown },
 { vlc_RButtonUp, (vlc_event_callback_t)VLCPlugin::onRButtonUp },
 { vlc_LButtonDblClk, (vlc_event_callback_t)VLCPlugin::onLButtonDblClk },
};
int i_callbacks_length = sizeof(p_event_callbacks) / sizeof(s_event_t);

HRESULT VLCPlugin::getVLC(libvlc_instance_t** pp_libvlc)
{
    extern HMODULE DllGetModule();

    if( ! isRunning() )
    {
        /*
        ** default initialization options
        */
        const char *ppsz_argv[32] = { };
        int   ppsz_argc = 0;

        char p_progpath[MAX_PATH];
        {
            TCHAR w_progpath[MAX_PATH];
            DWORD len = GetModuleFileName(DllGetModule(), w_progpath, MAX_PATH);
            if( len > 0 )
            {
                len = WideCharToMultiByte(CP_UTF8, 0, w_progpath, len, p_progpath,
                           sizeof(p_progpath)-1, NULL, NULL);
                if( len > 0 )
                {
                    p_progpath[len] = '\0';
                    ppsz_argv[0] = p_progpath;
                }
            }
        }

        ppsz_argv[ppsz_argc++] = "-vv";

        HKEY h_key;
        char p_pluginpath[MAX_PATH];
        if( RegOpenKeyEx( HKEY_LOCAL_MACHINE, TEXT("Software\\VideoLAN\\VLC"),
                          0, KEY_READ, &h_key ) == ERROR_SUCCESS )
        {
            DWORD i_type, i_data = MAX_PATH;
            TCHAR w_pluginpath[MAX_PATH];
            if( RegQueryValueEx( h_key, TEXT("InstallDir"), 0, &i_type,
                                 (LPBYTE)w_pluginpath, &i_data ) == ERROR_SUCCESS )
            {
                if( i_type == REG_SZ )
                {
                    if( WideCharToMultiByte(CP_UTF8, 0, w_pluginpath, -1, p_pluginpath,
                             sizeof(p_pluginpath)-sizeof("\\plugins")+1, NULL, NULL) )
                    {
                        strcat( p_pluginpath, "\\plugins" );
                        ppsz_argv[ppsz_argc++] = "--plugin-path";
                        ppsz_argv[ppsz_argc++] = p_pluginpath;
                    }
                }
            }
            RegCloseKey( h_key );
        }

        // make sure plugin isn't affected with VLC single instance mode
        ppsz_argv[ppsz_argc++] = "--no-one-instance";

        /* common settings */
        ppsz_argv[ppsz_argc++] = "--no-stats";
        ppsz_argv[ppsz_argc++] = "--no-media-library";
        ppsz_argv[ppsz_argc++] = "--ignore-config";
        ppsz_argv[ppsz_argc++] = "--intf=dummy";

        // loop mode is a configuration option only
        if( _b_autoloop )
            ppsz_argv[ppsz_argc++] = "--loop";

        libvlc_exception_t ex;
        libvlc_exception_init(&ex);

        _p_libvlc = libvlc_new(ppsz_argc, ppsz_argv, &ex);
        if( libvlc_exception_raised(&ex) )
        {
            *pp_libvlc = NULL;
            libvlc_exception_clear(&ex);
            return E_FAIL;
        }
        
        for( int i_counter = 0; i_counter < i_callbacks_length ; i_counter++ )
        {
         libvlc_exception_init(&ex);
         libvlc_instance_event_attach(_p_libvlc,
          p_event_callbacks[i_counter]._i_event,
          p_event_callbacks[i_counter]._pf_callback, this, &ex);
     
         if( libvlc_exception_raised(&ex) )
         {
          libvlc_exception_clear(&ex);
          return E_FAIL;
         }
        }

        // initial volume setting
        libvlc_audio_set_volume(_p_libvlc, _i_volume, NULL);
        if( _b_mute )
        {
            libvlc_audio_set_mute(_p_libvlc, TRUE, NULL);
        }

        // initial playlist item
        if( SysStringLen(_bstr_mrl) > 0 )
        {
            char *psz_mrl = NULL;

            if( SysStringLen(_bstr_baseurl) > 0 )
            {
                /*
                ** if the MRL a relative URL, we should end up with an absolute URL
                */
                LPWSTR abs_url = CombineURL(_bstr_baseurl, _bstr_mrl);
                if( NULL != abs_url )
                {
                    psz_mrl = CStrFromWSTR(CP_UTF8, abs_url, wcslen(abs_url));
                    CoTaskMemFree(abs_url);
                }
                else
                {
                    psz_mrl = CStrFromBSTR(CP_UTF8, _bstr_mrl);
                }
            }
            else
            {
                /*
                ** baseURL is empty, assume MRL is absolute
                */
                psz_mrl = CStrFromBSTR(CP_UTF8, _bstr_mrl);
            }
            if( NULL != psz_mrl )
            {
                const char *options[1];
                int i_options = 0;

                char timeBuffer[32];
                if( _i_time )
                {
                    snprintf(timeBuffer, sizeof(timeBuffer), ":start-time=%d", _i_time);
                    options[i_options++] = timeBuffer;
                }
                // add default target to playlist
                libvlc_playlist_add_extended_untrusted(_p_libvlc, psz_mrl, NULL, i_options, options, NULL);
                CoTaskMemFree(psz_mrl);
            }
        }
    }
    *pp_libvlc = _p_libvlc;
    return S_OK;
};

void VLCPlugin::setErrorInfo(REFIID riid, const char *description)
{
    vlcSupportErrorInfo->setErrorInfo( getClassID() == CLSID_VLCPlugin2 ?
        OLESTR("VideoLAN.VLCPlugin.2") : OLESTR("VideoLAN.VLCPlugin.1"),
        riid, description );
};

HRESULT VLCPlugin::onAmbientChanged(LPUNKNOWN pContainer, DISPID dispID)
{
    VARIANT v;
    switch( dispID )
    {
        case DISPID_AMBIENT_BACKCOLOR:
            VariantInit(&v);
            V_VT(&v) = VT_I4;
            if( SUCCEEDED(GetObjectProperty(pContainer, dispID, v)) )
            {
                setBackColor(V_I4(&v));
            }
            break;
        case DISPID_AMBIENT_DISPLAYNAME:
            break;
        case DISPID_AMBIENT_FONT:
            break;
        case DISPID_AMBIENT_FORECOLOR:
            break;
        case DISPID_AMBIENT_LOCALEID:
            break;
        case DISPID_AMBIENT_MESSAGEREFLECT:
            break;
        case DISPID_AMBIENT_SCALEUNITS:
            break;
        case DISPID_AMBIENT_TEXTALIGN:
            break;
        case DISPID_AMBIENT_USERMODE:
            VariantInit(&v);
            V_VT(&v) = VT_BOOL;
            if( SUCCEEDED(GetObjectProperty(pContainer, dispID, v)) )
            {
                setUserMode(V_BOOL(&v) != VARIANT_FALSE);
            }
            break;
        case DISPID_AMBIENT_UIDEAD:
            break;
        case DISPID_AMBIENT_SHOWGRABHANDLES:
            break;
        case DISPID_AMBIENT_SHOWHATCHING:
            break;
        case DISPID_AMBIENT_DISPLAYASDEFAULT:
            break;
        case DISPID_AMBIENT_SUPPORTSMNEMONICS:
            break;
        case DISPID_AMBIENT_AUTOCLIP:
            break;
        case DISPID_AMBIENT_APPEARANCE:
            break;
        case DISPID_AMBIENT_CODEPAGE:
            VariantInit(&v);
            V_VT(&v) = VT_I4;
            if( SUCCEEDED(GetObjectProperty(pContainer, dispID, v)) )
            {
                setCodePage(V_I4(&v));
            }
            break;
        case DISPID_AMBIENT_PALETTE:
            break;
        case DISPID_AMBIENT_CHARSET:
            break;
        case DISPID_AMBIENT_RIGHTTOLEFT:
            break;
        case DISPID_AMBIENT_TOPTOBOTTOM:
            break;
        case DISPID_UNKNOWN:
            /*
            ** multiple property change, look up the ones we are interested in
            */
            VariantInit(&v);
            V_VT(&v) = VT_BOOL;
            if( SUCCEEDED(GetObjectProperty(pContainer, DISPID_AMBIENT_USERMODE, v)) )
            {
                setUserMode(V_BOOL(&v) != VARIANT_FALSE);
            }
            VariantInit(&v);
            V_VT(&v) = VT_I4;
            if( SUCCEEDED(GetObjectProperty(pContainer, DISPID_AMBIENT_CODEPAGE, v)) )
            {
                setCodePage(V_I4(&v));
            }
            break;
    }
    return S_OK;
};

HRESULT VLCPlugin::onClose(DWORD dwSaveOption)
{
    if( isInPlaceActive() )
    {
        onInPlaceDeactivate();
    }
    if( isRunning() )
    {
        libvlc_instance_t* p_libvlc = _p_libvlc;

        IVLCLog *p_log;
        if( SUCCEEDED(vlcControl2->get_log(&p_log)) )
        {
            // make sure the log is disabled
            p_log->put_verbosity(-1);
            p_log->Release();
        }

        _p_libvlc = NULL;
        vlcDataObject->onClose();

        if( p_libvlc )
        {
         libvlc_exception_t ex;
         libvlc_exception_init(&ex);
 
         for( int i_counter = 0; i_counter < i_callbacks_length ; i_counter++ )
         {
          libvlc_exception_init(&ex);
          libvlc_instance_event_detach(_p_libvlc,
           p_event_callbacks[i_counter]._i_event,
           p_event_callbacks[i_counter]._pf_callback, this, &ex);
      
          if( libvlc_exception_raised(&ex) )
          {
           libvlc_exception_clear(&ex);
           return E_FAIL;
          }
         }

	 libvlc_release(p_libvlc);
        }
    }
    return S_OK;
};

BOOL VLCPlugin::isInPlaceActive(void)
{
    return (NULL != _inplacewnd);
};

HRESULT VLCPlugin::onActivateInPlace(LPMSG lpMesg, HWND hwndParent, LPCRECT lprcPosRect, LPCRECT lprcClipRect)
{
    RECT clipRect = *lprcClipRect;

    /*
    ** record keeping of control geometry within container
    */
    _posRect = *lprcPosRect;

    /*
    ** Create a window for in place activated control.
    ** the window geometry matches the control viewport
    ** within container so that embedded video is always
    ** properly displayed.
    */
    _inplacewnd = CreateWindow(_p_class->getInPlaceWndClassName(),
            TEXT("VLC Plugin In-Place Window"),
            WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
            lprcPosRect->left,
            lprcPosRect->top,
            lprcPosRect->right-lprcPosRect->left,
            lprcPosRect->bottom-lprcPosRect->top,
            hwndParent,
            0,
            _p_class->getHInstance(),
            NULL
           );

    if( NULL == _inplacewnd )
        return E_FAIL;

    SetWindowLongPtr(_inplacewnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    /* change cliprect coordinates system relative to window bounding rect */
    OffsetRect(&clipRect, -lprcPosRect->left, -lprcPosRect->top);

/*
    HRGN clipRgn = CreateRectRgnIndirect(&clipRect);
    SetWindowRgn(_inplacewnd, clipRgn, TRUE);
*/

    if( _b_usermode )
    {
        /* will run vlc if not done already */
        libvlc_instance_t* p_libvlc;
        HRESULT result = getVLC(&p_libvlc);
        if( FAILED(result) )
            return result;

        /* set internal video width and height */
        libvlc_video_set_size(p_libvlc,
            lprcPosRect->right-lprcPosRect->left,
            lprcPosRect->bottom-lprcPosRect->top,
            NULL );

        /* set internal video parent window */
        libvlc_video_set_parent(p_libvlc,
            reinterpret_cast<libvlc_drawable_t>(_inplacewnd), NULL);

        if( _b_autoplay & (libvlc_playlist_items_count(p_libvlc, NULL) > 0) )
        {
            libvlc_playlist_play(p_libvlc, 0, 0, NULL, NULL);
            fireOnPlayEvent();
        }
    }

    if( isVisible() )
        ShowWindow(_inplacewnd, SW_SHOW);

    return S_OK;
};

void VLCPlugin::onDestroy()
{
//    SetWindowLongPtr(_inplacewnd, GWLP_USERDATA, NULL);
 while( !_q_events.empty() )
 {
  _s_message_t *p_message = _q_events.front();
  _q_events.pop();
  delete p_message;
 }
}

HRESULT VLCPlugin::onInPlaceDeactivate(void)
{
    if( isRunning() )
    {
        libvlc_playlist_stop(_p_libvlc, NULL);
        fireOnStopEvent();
    }

    DestroyWindow(_inplacewnd);
    _inplacewnd = NULL;

    return S_OK;
};

void VLCPlugin::setVisible(BOOL fVisible)
{
    if( fVisible != _b_visible )
    {
        _b_visible = fVisible;
        if( isInPlaceActive() )
        {
            ShowWindow(_inplacewnd, fVisible ? SW_SHOW : SW_HIDE);
            if( fVisible )
                InvalidateRect(_inplacewnd, NULL, TRUE);
        }
        setDirty(TRUE);
        firePropChangedEvent(DISPID_Visible);
    }
};

void VLCPlugin::setVolume(int volume)
{
    if( volume < 0 )
        volume = 0;
    else if( volume > 200 )
        volume = 200;

    if( volume != _i_volume )
    {
        _i_volume = volume;
        if( isRunning() )
        {
            libvlc_audio_set_volume(_p_libvlc, _i_volume, NULL);
        }
        setDirty(TRUE);
    }
};

void VLCPlugin::setBackColor(OLE_COLOR backcolor)
{
    if( _i_backcolor != backcolor )
    {
        _i_backcolor = backcolor;
        if( isInPlaceActive() )
        {

        }
        setDirty(TRUE);
    }
};

void VLCPlugin::setPausedBitmap(int hbitmap)
{
 if( NULL != hbitmap )
 {
  BITMAPINFO info;
  HWND hwnd=( _inplacewnd )? _inplacewnd : GetDesktopWindow();
  HDC hicTargetDev = GetDC( hwnd );

  memset( &info, 0 ,sizeof(BITMAPINFO) );
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
 
  if( GetDIBits( hicTargetDev, (HBITMAP)hbitmap, 0, 0, NULL, &info, DIB_RGB_COLORS ) )
  {
   SIZEL picSize = { info.bmiHeader.biWidth, info.bmiHeader.biHeight };

   HDC hDC = CreateCompatibleDC( hicTargetDev );
   HBITMAP hTmpBmp = (HBITMAP) SelectObject( hDC, (HBITMAP)hbitmap );
   HDC hDrawDC = CreateCompatibleDC( hicTargetDev );
   HBITMAP hDuplicate = CreateCompatibleBitmap( hicTargetDev, picSize.cx, picSize.cy );
   hDuplicate = (HBITMAP) SelectObject( hDrawDC, hDuplicate );
   BitBlt( hDrawDC, 0, 0, picSize.cx, picSize.cy, hDC, 0, 0, SRCCOPY);
   SelectObject( hDC, hTmpBmp );
   hDuplicate = (HBITMAP) SelectObject( hDrawDC, hDuplicate );
   DeleteDC( hDC );
   DeleteDC( hDrawDC );

  
   if( hDuplicate )
   {
    _paused_bitmap = hDuplicate;
    _paused_bitmap_size = picSize;
 
    if( isRunning() )
     libvlc_set_paused_bitmap( _p_libvlc, (int)hDuplicate, 
      (int)picSize.cx, (int)picSize.cy );
   }
  }
  ReleaseDC( hwnd, hicTargetDev );
 }
}

int VLCPlugin::getPausedBitmap(void)
{
 if( isRunning() )
  return libvlc_get_paused_bitmap( _p_libvlc );
 return 0;
}


void VLCPlugin::setTime(int seconds)
{
    if( seconds < 0 )
        seconds = 0;

    if( seconds != _i_time )
    {
        setStartTime(_i_time);
        if( isRunning() )
        {
            libvlc_media_player_t *p_md = libvlc_playlist_get_media_player(_p_libvlc, NULL);
            if( NULL != p_md )
            {
                libvlc_media_player_set_time(p_md, _i_time, NULL);
                libvlc_media_player_release(p_md);
            }
        }
    }
};

void VLCPlugin::setFocus(BOOL fFocus)
{
    if( fFocus )
        SetActiveWindow(_inplacewnd);
};

BOOL VLCPlugin::hasFocus(void)
{
    return GetActiveWindow() == _inplacewnd;
};

extern "C" void f_write_log(char *,...);
void VLCPlugin::onDraw(DVTARGETDEVICE * ptd, HDC hicTargetDev,
        HDC hdcDraw, LPCRECTL lprcBounds, LPCRECTL lprcWBounds)
{
    if( isVisible() )
    {
        long width = lprcBounds->right-lprcBounds->left;
        long height = lprcBounds->bottom-lprcBounds->top;

        RECT bounds = { lprcBounds->left, lprcBounds->top, lprcBounds->right, lprcBounds->bottom };

        if( _p_pict == NULL && isUserMode() )
        {
          if(_paused_bitmap)
          {
            HDC hdc = CreateCompatibleDC( hicTargetDev );
            HBITMAP hbitmap;
            if( width != _render_bitmap_size.cx || 
             height != _render_bitmap_size.cy )
            {
             hbitmap = (HBITMAP) SelectObject( hdc, _paused_bitmap );
             StretchBlt( hdcDraw, 0, 0, width, height, hdc, 0, 0,
              _paused_bitmap_size.cx, _paused_bitmap_size.cy, SRCCOPY );
            }
            else 
            {
             hbitmap = (HBITMAP) SelectObject( hdc, _render_bitmap );
             BitBlt( hdcDraw, 0, 0, width, height, hdc, 0, 0, SRCCOPY );
            }
            SelectObject( hdc, hbitmap );
            DeleteDC( hdc );
          }  
          else
          {
            /* VLC is in user mode, just draw background color */
            COLORREF colorref = RGB(0, 0, 0);
            OleTranslateColor(_i_backcolor, (HPALETTE)GetStockObject(DEFAULT_PALETTE), &colorref);
            if( colorref != RGB(0, 0, 0) )
            {
                /* custom background */
                HBRUSH colorbrush = CreateSolidBrush(colorref);
                FillRect(hdcDraw, &bounds, colorbrush);
                DeleteObject((HANDLE)colorbrush);
            }
            else
            {
                /* black background */
                FillRect(hdcDraw, &bounds, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
          }
        }
        else
        {
            /* VLC is in design mode, draw the VLC logo */
            LPPICTURE pict;

            if(!isUserMode())
            {
             FillRect(hdcDraw, &bounds, (HBRUSH)GetStockObject(WHITE_BRUSH));
             pict = _p_class->getInPlacePict();
            }
            else pict = getPicture();
            if( NULL != pict )
            {
                OLE_XSIZE_HIMETRIC picWidth;
                OLE_YSIZE_HIMETRIC picHeight;

                pict->get_Width(&picWidth);
                pict->get_Height(&picHeight);

                SIZEL picSize = { picWidth, picHeight };

                if( NULL != hicTargetDev )
                {
                    DPFromHimetric(hicTargetDev, (LPPOINT)&picSize, 1);
                }
                else if( NULL != (hicTargetDev = CreateDevDC(ptd)) )
                {
                    DPFromHimetric(hicTargetDev, (LPPOINT)&picSize, 1);
                    DeleteDC(hicTargetDev);
                }

                if( picSize.cx > width-4 )
                    picSize.cx = width-4;
                if( picSize.cy > height-4 )
                    picSize.cy = height-4;

                LONG dstX = lprcBounds->left+(width-picSize.cx)/2;
                LONG dstY = lprcBounds->top+(height-picSize.cy)/2;

                if( NULL != lprcWBounds )
                {
                    RECT wBounds = { lprcWBounds->left, lprcWBounds->top, lprcWBounds->right, lprcWBounds->bottom };
                    pict->Render(hdcDraw, dstX, dstY, picSize.cx, picSize.cy,
                            0L, picHeight, picWidth, -picHeight, &wBounds);
                }
                else
                {
                  if(isUserMode())
                  {
                   HDC hdc = CreateCompatibleDC( hicTargetDev );
                   HBITMAP hbitmap = CreateCompatibleBitmap( hicTargetDev, picSize.cx, picSize.cy );
                   hbitmap = (HBITMAP) SelectObject( hdc, hbitmap );
                   pict->Render(hdc, 0, 0, picSize.cx, picSize.cy,
                        0L, picHeight, picWidth, -picHeight, NULL);
                   StretchBlt( hdcDraw, 0, 0, width, height, hdc, 0, 0,
                    picSize.cx, picSize.cy, SRCCOPY );
                   hbitmap = (HBITMAP) SelectObject( hdc, hbitmap );
                   if( hbitmap ) DeleteObject( hbitmap );
                   DeleteDC( hdc );
                  }
                  else
                   pict->Render(hdcDraw, dstX, dstY, picSize.cx, picSize.cy,
                        0L, picHeight, picWidth, -picHeight, NULL);
                }

                pict->Release();
            }

            if(!isUserMode())
            {
            SelectObject(hdcDraw, GetStockObject(BLACK_BRUSH));

            MoveToEx(hdcDraw, bounds.left, bounds.top, NULL);
            LineTo(hdcDraw, bounds.left+width-1, bounds.top);
            LineTo(hdcDraw, bounds.left+width-1, bounds.top+height-1);
            LineTo(hdcDraw, bounds.left, bounds.top+height-1);
            LineTo(hdcDraw, bounds.left, bounds.top);
            }
        }
    }
};

void VLCPlugin::onPaint(HDC hdc, const RECT &bounds, const RECT &clipRect)
{
    if( isVisible() )
    {
        /* if VLC is in design mode, draw control logo */
        HDC hdcDraw = CreateCompatibleDC(hdc);
        if( NULL != hdcDraw )
        {
            SIZEL size = getExtent();
            DPFromHimetric(hdc, (LPPOINT)&size, 1);
            RECTL posRect = { 0, 0, size.cx, size.cy };

            int width = bounds.right-bounds.left;
            int height = bounds.bottom-bounds.top;

            HBITMAP hBitmap = CreateCompatibleBitmap(hdc, width, height);
            if( NULL != hBitmap )
            {
                HBITMAP oldBmp = (HBITMAP)SelectObject(hdcDraw, hBitmap);

                if( (size.cx != width) || (size.cy != height) )
                {
                    // needs to scale canvas
                    SetMapMode(hdcDraw, MM_ANISOTROPIC);
                    SetWindowExtEx(hdcDraw, size.cx, size.cy, NULL);
                    SetViewportExtEx(hdcDraw, width, height, NULL);
                }

                onDraw(NULL, hdc, hdcDraw, &posRect, NULL);

                SetMapMode(hdcDraw, MM_TEXT);
                BitBlt(hdc, bounds.left, bounds.top,
                        width, height,
                        hdcDraw, 0, 0,
                        SRCCOPY);

                SelectObject(hdcDraw, oldBmp);
                if( _p_pict == NULL && isUserMode() && _paused_bitmap &&
                 ( width != _render_bitmap_size.cx || 
                   height != _render_bitmap_size.cy )
                 )
                {
                 if( _render_bitmap ) DeleteObject( _render_bitmap );
                 _render_bitmap = hBitmap;
                 _render_bitmap_size.cx = width;
                 _render_bitmap_size.cy = height;
                }
                else DeleteObject(hBitmap);
            }
            DeleteDC(hdcDraw);
        }
    }
};

void VLCPlugin::onPositionChange(LPCRECT lprcPosRect, LPCRECT lprcClipRect)
{
    RECT clipRect = *lprcClipRect;

    //RedrawWindow(GetParent(_inplacewnd), &_posRect, NULL, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);

    /*
    ** record keeping of control geometry within container
    */
    _posRect = *lprcPosRect;

    /*
    ** change in-place window geometry to match clipping region
    */
    SetWindowPos(_inplacewnd, NULL,
            lprcPosRect->left,
            lprcPosRect->top,
            lprcPosRect->right-lprcPosRect->left,
            lprcPosRect->bottom-lprcPosRect->top,
            SWP_NOACTIVATE|
            SWP_NOCOPYBITS|
            SWP_NOZORDER|
            SWP_NOOWNERZORDER );

    /* change cliprect coordinates system relative to window bounding rect */
    OffsetRect(&clipRect, -lprcPosRect->left, -lprcPosRect->top);
/*
    HRGN clipRgn = CreateRectRgnIndirect(&clipRect);
    SetWindowRgn(_inplacewnd, clipRgn, FALSE);
*/

    //RedrawWindow(_videownd, &posRect, NULL, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    if( isRunning() )
    {
        libvlc_video_set_size(_p_libvlc,
            lprcPosRect->right-lprcPosRect->left,
            lprcPosRect->bottom-lprcPosRect->top,
            NULL );
    }
};

void VLCPlugin::onTimer()
{
 _s_message_t message = popMessage();
 switch( message._i_type )
 {
  case vlc_InputThreadFinished:
   fireInputThreadFinished();
  break;
  
  case vlc_OutputThreadStarted:
   fireOutputThreadStarted();
  break;

  case vlc_InputThreadStopResponding:
   fireInputThreadStopResponding();
  break;

  case vlc_InputThreadResumeResponding:
   fireInputThreadResumeResponding();
  break;

  case vlc_MouseMove:
   fireMouseMove( message.u.mouse_position_changed.x,
    message.u.mouse_position_changed.y );
  break;

  case vlc_NCMouseMove:
   fireNCMouseMove( message.u.mouse_position_changed.x,
    message.u.mouse_position_changed.y );
  break;

  case vlc_LButtonDown:
   fireLButtonDown();
  break;

  case vlc_LButtonUp:
   fireLButtonUp();
  break;

  case vlc_MButtonDown:
   fireMButtonDown();
  break;

  case vlc_MButtonUp:
   fireMButtonUp();
  break;

  case vlc_RButtonDown:
   fireRButtonDown();
  break;

  case vlc_RButtonUp:
   fireRButtonUp();
  break;

  case vlc_LButtonDblClk:
   fireLButtonDblClk();
  break;

 }

}

void VLCPlugin::freezeEvents(BOOL freeze)
{
    vlcConnectionPointContainer->freezeEvents(freeze);
};

void VLCPlugin::firePropChangedEvent(DISPID dispid)
{
    vlcConnectionPointContainer->firePropChangedEvent(dispid);
};

void VLCPlugin::fireOnPlayEvent(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_PlayEvent, &dispparamsNoArgs);
};

void VLCPlugin::fireOnPauseEvent(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_PauseEvent, &dispparamsNoArgs);
};

void VLCPlugin::fireOnStopEvent(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_StopEvent, &dispparamsNoArgs);
};

void VLCPlugin::fireInputThreadFinished(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_InputThreadFinished, &dispparamsNoArgs);
};

void VLCPlugin::fireOutputThreadStarted(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_OutputThreadStarted, &dispparamsNoArgs);
};

void VLCPlugin::fireInputThreadStopResponding(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_InputThreadStopResponding, &dispparamsNoArgs);
};

void VLCPlugin::fireInputThreadResumeResponding(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_InputThreadResumeResponding, &dispparamsNoArgs);
};

void VLCPlugin::fireMouseMove(int x, int y)
{
    VARIANTARG args[2];
    memset( args, 0, sizeof(VARIANTARG)*2 );
    args[0].vt = VT_INT;
    args[0].intVal = x;
    args[1].vt = VT_INT;
    args[1].intVal = y;
    DISPPARAMS dispparamsArgs = {args, NULL, 2, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_MouseMove, &dispparamsArgs);
};

void VLCPlugin::fireNCMouseMove(int x, int y)
{
    VARIANTARG args[2];
    memset( args, 0, sizeof(VARIANTARG)*2 );
    args[0].vt = VT_INT;
    args[0].intVal = x;
    args[1].vt = VT_INT;
    args[1].intVal = y;
    DISPPARAMS dispparamsArgs = {args, NULL, 2, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_NCMouseMove, &dispparamsArgs);
};

void VLCPlugin::fireLButtonDown(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_LButtonDown, &dispparamsNoArgs);
};

void VLCPlugin::fireLButtonUp(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_LButtonUp, &dispparamsNoArgs);
};

void VLCPlugin::fireMButtonDown(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_MButtonDown, &dispparamsNoArgs);
};

void VLCPlugin::fireMButtonUp(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_MButtonUp, &dispparamsNoArgs);
};

void VLCPlugin::fireRButtonDown(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_RButtonDown, &dispparamsNoArgs);
};

void VLCPlugin::fireRButtonUp(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_RButtonUp, &dispparamsNoArgs);
};

void VLCPlugin::fireLButtonDblClk(void)
{
    DISPPARAMS dispparamsNoArgs = {NULL, NULL, 0, 0};
    vlcConnectionPointContainer->fireEvent(DISPID_LButtonDblClk, &dispparamsNoArgs);
};



void VLCPlugin::onInputThreadFinished(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_InputThreadFinished;
  p_this->pushMessage( message );
 #else
  p_this->fireInputThreadFinished();
 #endif
};

void VLCPlugin::onOutputThreadStarted(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_OutputThreadStarted;
  p_this->pushMessage( message );
 #else
  p_this->fireOutputThreadStarted();
 #endif
};

void VLCPlugin::onInputThreadStopResponding(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_InputThreadStopResponding;
  p_this->pushMessage( message );
 #else
  p_this->fireInputThreadStopResponding();
 #endif
};

void VLCPlugin::onInputThreadResumeResponding(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_InputThreadResumeResponding;
  p_this->pushMessage( message );
 #else
  p_this->fireInputThreadResumeResponding();
 #endif
};

void VLCPlugin::onMouseMove(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_MouseMove;
  message.u.mouse_position_changed.x = p_event->u.mouse_position_changed.x;
  message.u.mouse_position_changed.y = p_event->u.mouse_position_changed.y;
  if( !p_this->findMessage( message ))
   p_this->pushMessage( message );
 #else
  p_this->fireMouseMove(p_event->u.mouse_position_changed.x,
   p_event->u.mouse_position_changed.y);
 #endif
}

void VLCPlugin::onNCMouseMove(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_NCMouseMove;
  message.u.mouse_position_changed.x = p_event->u.mouse_position_changed.x;
  message.u.mouse_position_changed.y = p_event->u.mouse_position_changed.y;
  if( !p_this->findMessage( message ))
   p_this->pushMessage( message );
 #else
  p_this->fireNCMouseMove(p_event->u.mouse_position_changed.x,
   p_event->u.mouse_position_changed.y);
 #endif
}

void VLCPlugin::onLButtonDown(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
  _s_message_t message;
  message._i_type = vlc_LButtonDown;
  p_this->pushMessage( message );
 #else
  p_this->fireLButtonDown();
 #endif
}

void VLCPlugin::onLButtonUp(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_LButtonUp;
 p_this->pushMessage( message );
 #else
  p_this->fireLButtonUp();
 #endif
}

void VLCPlugin::onMButtonDown(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_MButtonDown;
 p_this->pushMessage( message );
 #else
  p_this->fireMButtonDown();
 #endif
}

void VLCPlugin::onMButtonUp(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_MButtonUp;
 p_this->pushMessage( message );
 #else
  p_this->fireMButtonUp();
 #endif
}

void VLCPlugin::onRButtonDown(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_RButtonDown;
 p_this->pushMessage( message );
 #else
  p_this->fireRButtonDown();
 #endif
}

void VLCPlugin::onRButtonUp(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_RButtonUp;
 p_this->pushMessage( message );
 #else
  p_this->fireRButtonUp();
 #endif
}

void VLCPlugin::onLButtonDblClk(
 vlc_event_t *p_event,void *p_data)
{
 VLCPlugin *p_this=(VLCPlugin*) p_data;
 #ifdef _D_SYNCHRONIZE
 _s_message_t message;
 message._i_type = vlc_LButtonDblClk;
 p_this->pushMessage( message );
 #else
  p_this->fireLButtonDblClk();
 #endif
}

