#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include "steamcompmgr.hpp"

#include <libinput.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <set>
#include <string>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    class CHeadlessConnector final : public CBaseBackendConnector
    {
    public:
        CHeadlessConnector()
        {
        }
        virtual ~CHeadlessConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return false;
        }
        virtual bool IsHDRActive() const override
        {
            return false;
        }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
        {
            return m_HDRInfo;
        }
		virtual bool IsVRRActive() const override
		{
			return false;
		}
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }

        virtual bool SupportsVRR() const override
        {
            return false;
        }

        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{};
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;
			*outputEncodingColorimetry = displaycolorimetry_709;
			*outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override
        {
            return "Headless";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Virtual Display";
        }

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
			// Composite the frame to output images so scanout export can send it.
			// Without this, the headless backend never produces frames for capture.
			std::optional oCompositeResult = vulkan_composite( (FrameInfo_t *)pFrameInfo, nullptr, false );
			if ( !oCompositeResult )
				return -EINVAL;

			vulkan_wait( *oCompositeResult, true );

			GetVBlankTimer().UpdateWasCompositing( true );
			GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

			return 0;
		}

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
    };

	class CHeadlessBackend final : public CBaseBackend
	{
	public:
		CHeadlessBackend()
		{
		}

		virtual ~CHeadlessBackend()
		{
			m_bInputThreadRunning = false;
			if ( m_InputThread.joinable() )
				m_InputThread.join();
			if ( m_pPathLibInput )
				libinput_unref( m_pPathLibInput );
		}

		virtual bool Init() override
		{
			g_nOutputWidth = g_nPreferredOutputWidth;
			g_nOutputHeight = g_nPreferredOutputHeight;
			g_nOutputRefresh = g_nNestedRefresh;

			if ( g_nOutputHeight == 0 )
			{
				if ( g_nOutputWidth != 0 )
				{
					fprintf( stderr, "Cannot specify -W without -H\n" );
					return false;
				}
				g_nOutputHeight = 720;
			}
			if ( g_nOutputWidth == 0 )
				g_nOutputWidth = g_nOutputHeight * 16 / 9;
			if ( g_nOutputRefresh == 0 )
				g_nOutputRefresh = ConvertHztomHz( 60 );

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
				return false;
			}

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

			return true;
		}

		virtual bool PostInit() override
		{
			// Initialize path-based libinput (no udev daemon needed) and
			// start a background thread that polls /dev/input/ for new
			// virtual devices created by Sunshine via uinput.
			static const libinput_interface iface = {
				.open_restricted = []( const char *path, int flags, void * ) -> int {
					return open( path, flags );
				},
				.close_restricted = []( int fd, void * ) {
					close( fd );
				},
			};

			m_pPathLibInput = libinput_path_create_context( &iface, nullptr );
			if ( !m_pPathLibInput )
			{
				fprintf( stderr, "[headless] libinput_path_create_context failed\n" );
				return true; // non-fatal
			}

			fprintf( stderr, "[headless] libinput path context created — polling /dev/input/ for devices\n" );

			// Background thread: scan /dev/input/ for container devices,
			// add new ones to libinput, dispatch events to wlserver.
			m_bInputThreadRunning = true;
			m_InputThread = std::thread( [this]() {
				std::set<std::string> knownDevices;

				while ( m_bInputThreadRunning )
				{
					// Scan for new devices
					DIR *d = opendir( "/dev/input" );
					if ( d )
					{
						struct dirent *ent;
						while ( ( ent = readdir( d ) ) )
						{
							if ( strncmp( ent->d_name, "event", 5 ) != 0 )
								continue;

							std::string name( ent->d_name );
							if ( knownDevices.count( name ) )
								continue;

							// Check if this is a container device by reading phys
							char physPath[PATH_MAX];
							snprintf( physPath, sizeof(physPath),
								"/sys/class/input/%s/device/phys", ent->d_name );
							FILE *f = fopen( physPath, "r" );
							if ( !f )
								continue;
							char phys[256] = {};
							if ( !fgets( phys, sizeof(phys), f ) ) { fclose(f); continue; }
							fclose( f );
							// Strip newline
							char *nl = strchr( phys, '\n' );
							if ( nl ) *nl = '\0';

							if ( strncmp( phys, "container-", 10 ) != 0 )
								continue;

							char devPath[PATH_MAX];
							snprintf( devPath, sizeof(devPath), "/dev/input/%s", ent->d_name );

							struct libinput_device *dev = libinput_path_add_device( m_pPathLibInput, devPath );
							if ( dev )
							{
								fprintf( stderr, "[headless] added input device: %s (%s)\n", devPath, phys );
								knownDevices.insert( name );
							}
						}
						closedir( d );

						// Remove stale entries: if a known device no longer exists
						// in /dev/input/, remove it from the set so it gets re-added
						// when Moonlight reconnects and Sunshine recreates it.
						for ( auto it = knownDevices.begin(); it != knownDevices.end(); )
						{
							char checkPath[PATH_MAX];
							snprintf( checkPath, sizeof(checkPath), "/dev/input/%s", it->c_str() );
							if ( access( checkPath, F_OK ) != 0 )
							{
								fprintf( stderr, "[headless] removed stale device: /dev/input/%s\n", it->c_str() );
								it = knownDevices.erase( it );
							}
							else
								++it;
						}
					}

					// Dispatch pending events
					libinput_dispatch( m_pPathLibInput );
					struct libinput_event *ev;
					while ( ( ev = libinput_get_event( m_pPathLibInput ) ) )
					{
						libinput_event_type type = libinput_event_get_type( ev );
						switch ( type )
						{
							case LIBINPUT_EVENT_POINTER_MOTION:
							{
								auto *pe = libinput_event_get_pointer_event( ev );
								wlserver_lock();
								wlserver_mousemotion( libinput_event_pointer_get_dx( pe ),
								                      libinput_event_pointer_get_dy( pe ), 0 );
								wlserver_unlock();
								break;
							}
							case LIBINPUT_EVENT_POINTER_BUTTON:
							{
								auto *pe = libinput_event_get_pointer_event( ev );
								wlserver_lock();
								wlserver_mousebutton( libinput_event_pointer_get_button( pe ),
								                      libinput_event_pointer_get_button_state( pe ) == LIBINPUT_BUTTON_STATE_PRESSED, 0 );
								wlserver_unlock();
								break;
							}
							case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
							{
								auto *pe = libinput_event_get_pointer_event( ev );
								double sx = 0, sy = 0;
								if ( libinput_event_pointer_has_axis( pe, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) )
									sx = libinput_event_pointer_get_scroll_value_v120( pe, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) / 120.0;
								if ( libinput_event_pointer_has_axis( pe, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) )
									sy = libinput_event_pointer_get_scroll_value_v120( pe, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) / 120.0;
								if ( sx != 0 || sy != 0 )
								{
									wlserver_lock();
									wlserver_mousewheel( sx, sy, 0 );
									wlserver_unlock();
								}
								break;
							}
							case LIBINPUT_EVENT_KEYBOARD_KEY:
							{
								auto *ke = libinput_event_get_keyboard_event( ev );
								wlserver_lock();
								wlserver_key( libinput_event_keyboard_get_key( ke ),
								              libinput_event_keyboard_get_key_state( ke ) == LIBINPUT_KEY_STATE_PRESSED, 0 );
								wlserver_unlock();
								break;
							}
							case LIBINPUT_EVENT_TOUCH_DOWN:
							{
								auto *te = libinput_event_get_touch_event( ev );
								double x = libinput_event_touch_get_x_transformed( te, 1 );
								double y = libinput_event_touch_get_y_transformed( te, 1 );
								wlserver_lock();
								wlserver_touchdown( x, y, libinput_event_touch_get_slot( te ),
								                    libinput_event_touch_get_time( te ) );
								wlserver_unlock();
								break;
							}
							case LIBINPUT_EVENT_TOUCH_MOTION:
							{
								auto *te = libinput_event_get_touch_event( ev );
								double x = libinput_event_touch_get_x_transformed( te, 1 );
								double y = libinput_event_touch_get_y_transformed( te, 1 );
								wlserver_lock();
								wlserver_touchmotion( x, y, libinput_event_touch_get_slot( te ),
								                      libinput_event_touch_get_time( te ) );
								wlserver_unlock();
								break;
							}
							case LIBINPUT_EVENT_TOUCH_UP:
							{
								auto *te = libinput_event_get_touch_event( ev );
								wlserver_lock();
								wlserver_touchup( libinput_event_touch_get_slot( te ),
								                  libinput_event_touch_get_time( te ) );
								wlserver_unlock();
								break;
							}
							default:
								break;
						}
						libinput_event_destroy( ev );
					}

					// Use libinput's fd for event-driven wakeup instead of fixed polling
					struct pollfd pfd = { libinput_get_fd( m_pPathLibInput ), POLLIN, 0 };
					poll( &pfd, 1, 500 ); // wake on events, 500ms max for device scan
				}
			});

			return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_GENERAL;
		}
		virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
		{
			*pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
			*pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
		}
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
		}

		virtual bool PollState() override
		{
			return false;
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return new CBaseBackendFb();
		}

		virtual bool UsesModifiers() const override
		{
			return true;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			// Headless backend has no real display — use LINEAR for all formats.
			// LINEAR is universally importable by any DMA-BUF consumer.
			static const uint64_t s_LinearModifier = DRM_FORMAT_MOD_LINEAR;
			return std::span<const uint64_t>{ &s_LinearModifier, 1 };
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
				return &m_Connector;

			return nullptr;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return false;
		}

		virtual bool SupportsTearing() const override
		{
			return false;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}

        virtual bool IsSessionBased() const override
		{
			return false;
		}

		virtual bool SupportsExplicitSync() const override
		{
			return true;
		}

		virtual bool IsPaused() const override
		{
			return false;
		}

		virtual bool IsVisible() const override
		{
			return true;
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			return uvecSize;
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
		}

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        CHeadlessConnector m_Connector;
        struct libinput *m_pPathLibInput = nullptr;
        std::thread m_InputThread;
        bool m_bInputThreadRunning = false;
	};

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CHeadlessBackend>()
	{
		return Set( new CHeadlessBackend{} );
	}

}