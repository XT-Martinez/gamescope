#include "backend.h"
#include "main.hpp"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include "steamcompmgr.hpp"
#include "color_helpers.h"
#include "edid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <libinput.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <set>
#include <string>
#include <map>
#include <mutex>
#include <vector>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

extern bool g_bForceHDR10OutputDebug;
extern int g_nDynamicRefreshHz;
extern gamescope::ConVar<bool> cv_hdr_enabled;

namespace gamescope
{
	static constexpr float k_flHeadlessDefaultDpi = 220.0f;

	static int HeadlessPhysicalMmForPixels( int nPixels )
	{
		return std::max( 1, int( std::lround( ( float( nPixels ) * 25.4f ) / k_flHeadlessDefaultDpi ) ) );
	}

    class CHeadlessConnector final : public CBaseBackendConnector
    {
    public:
        CHeadlessConnector()
        {
            // Headless display always advertises HDR capability.
            // Whether HDR is used is controlled by cv_hdr_enabled at the compositor level.
            m_HDRInfo.bExposeHDRSupport = true;
            m_HDRInfo.eOutputEncodingEOTF = EOTF_PQ;
            m_HDRInfo.uMaxContentLightLevel = nits_to_u16( 1000.0f );
            m_HDRInfo.uMaxFrameAverageLuminance = nits_to_u16( 800.0f );
            m_HDRInfo.uMinContentLightLevel = nits_to_u16_dark( 0.01f );
        }

        void UpdateMode( uint32_t uWidth, uint32_t uHeight, uint32_t uRefreshHz )
        {
            m_CurrentMode.uWidth = uWidth;
            m_CurrentMode.uHeight = uHeight;
            m_CurrentMode.uRefresh = uRefreshHz;
            m_Edid = GenerateSimpleEdid( uWidth, uHeight );
        }
        virtual ~CHeadlessConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
			return GAMESCOPE_SCREEN_TYPE_EXTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return m_HDRInfo.IsHDR10();
        }
        virtual bool IsHDRActive() const override
        {
            return g_bOutputHDREnabled;
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
            return std::span<const BackendMode>{ &m_CurrentMode, 1 };
        }

        virtual bool SupportsVRR() const override
        {
            return false;
        }

        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{ m_Edid.data(), m_Edid.size() };
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            if ( !g_customRefreshRates.empty() )
                return std::span<const uint32_t>{ g_customRefreshRates.data(), g_customRefreshRates.size() };
            if ( !m_FallbackRefreshRates.empty() )
                return std::span<const uint32_t>{ m_FallbackRefreshRates.data(), m_FallbackRefreshRates.size() };
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;

			if ( bHDR10 && GetHDRInfo().IsHDR10() )
			{
				*outputEncodingColorimetry = displaycolorimetry_2020;
				*outputEncodingEOTF = GetHDRInfo().eOutputEncodingEOTF;
			}
			else
			{
				*outputEncodingColorimetry = displaycolorimetry_709;
				*outputEncodingEOTF = EOTF_Gamma22;
			}
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
        BackendMode m_CurrentMode{};
        std::vector<uint8_t> m_Edid;
        std::vector<uint32_t> m_FallbackRefreshRates;

        friend class CHeadlessBackend;
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

			// If --custom-refresh-rates wasn't passed, parse CUSTOM_REFRESH_RATES
			// env var as a fallback (gamescope-session-plus may not forward it).
			if ( g_customRefreshRates.empty() )
			{
				const char *pszRates = getenv( "CUSTOM_REFRESH_RATES" );
				if ( pszRates && *pszRates )
				{
					char *pszCopy = strdup( pszRates );
					char *token = strtok( pszCopy, "," );
					while ( token )
					{
						int rate = atoi( token );
						if ( rate > 0 )
							m_Connector.m_FallbackRefreshRates.push_back( (uint32_t)rate );
						token = strtok( nullptr, "," );
					}
					free( pszCopy );
					if ( !m_Connector.m_FallbackRefreshRates.empty() )
						fprintf( stderr, "[headless] parsed %zu refresh rates from CUSTOM_REFRESH_RATES env\n",
							m_Connector.m_FallbackRefreshRates.size() );
				}
			}

			if ( g_nOutputRefresh == 0 )
			{
				auto validRefreshRates = m_Connector.GetValidDynamicRefreshRates();
				if ( !validRefreshRates.empty() )
				{
					uint32_t uInitialRefreshHz = *std::max_element( validRefreshRates.begin(), validRefreshRates.end() );
					g_nOutputRefresh = ConvertHztomHz( uInitialRefreshHz );
					fprintf( stderr, "[headless] using initial refresh %uHz from configured dynamic refresh rates\n",
						uInitialRefreshHz );
				}
				else
				{
					g_nOutputRefresh = ConvertHztomHz( 60 );
				}
			}

			m_Connector.UpdateMode( g_nOutputWidth, g_nOutputHeight,
				ConvertmHzToHz( (uint32_t)g_nOutputRefresh ) );

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
				return false;
			}

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

			const struct wlserver_output_info outputInfo = {
				.description = "Gamescope virtual external display",
				.phys_width = HeadlessPhysicalMmForPixels( g_nOutputWidth ),
				.phys_height = HeadlessPhysicalMmForPixels( g_nOutputHeight ),
			};
			wlserver_lock();
			wlserver_set_output_info( &outputInfo );
			wlserver_unlock();

			return true;
		}

		virtual bool PostInit() override
		{
			// Write initial EDID so Steam can detect display capabilities (HDR, etc.)
			HackUpdatePatchedEdid();

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
				auto scan_devices = [&]() {
					DIR *d = opendir( "/dev/input" );
					if ( !d )
						return;

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
						{
							++it;
						}
					}
				};

				const auto scanInterval = std::chrono::milliseconds( 500 );
				auto tNextScan = std::chrono::steady_clock::now();

				while ( m_bInputThreadRunning )
				{
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

					auto tNow = std::chrono::steady_clock::now();
					if ( tNow >= tNextScan )
					{
						scan_devices();
						tNextScan = tNow + scanInterval;
					}

					// Use libinput's fd for event-driven wakeup and only rescan
					// devices periodically, so high-rate pointer motion does not
					// pay the cost of a /dev/input scan on every wakeup.
					int timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::max( tNextScan - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero() ) ).count();
					struct pollfd pfd = { libinput_get_fd( m_pPathLibInput ), POLLIN, 0 };
					poll( &pfd, 1, timeoutMs );
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
			if ( !wlserver.pending_output_mode.pending.load( std::memory_order_acquire ) )
				return false;

			uint32_t width = wlserver.pending_output_mode.width;
			uint32_t height = wlserver.pending_output_mode.height;
			int32_t refresh_mhz = wlserver.pending_output_mode.refresh_mhz;
			wlserver.pending_output_mode.pending.store( false, std::memory_order_relaxed );

			fprintf( stderr, "[headless] applying dynamic mode: %ux%u@%dHz\n",
				width, height, gamescope::ConvertmHzToHz( refresh_mhz ) );

			g_nOutputWidth = width;
			g_nOutputHeight = height;
			g_nOutputRefresh = refresh_mhz;

			m_Connector.UpdateMode( width, height,
				gamescope::ConvertmHzToHz( (uint32_t)refresh_mhz ) );

			vulkan_remake_output_images();

			wlserver_lock();
			for ( size_t i = 0; i < wlserver.wlr.xwayland_servers.size(); i++ )
				wlserver_set_xwayland_server_mode( i, width, height, refresh_mhz );
			wlserver_update_output_manager();
			wlserver_unlock();

			return true;
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
			// Headless has no display planes to constrain modifiers, so offer
			// every modifier the GPU can sample for this format: clients (Steam,
			// games) then render to tiled/compressed buffers instead of LINEAR,
			// and output images are created tiled and exported with their real
			// modifier, which scanout export consumers (Sunshine) import as-is.
			// Queried from Vulkan directly rather than via
			// GetSupportedSampleModifiers(), which is built from this function
			// during vulkan_init_formats() and is empty until then.  LINEAR is
			// always included so the list is never empty.
			std::scoped_lock lock( m_ModifierMutex );
			auto iter = m_SupportedModifiers.find( uDrmFormat );
			if ( iter == m_SupportedModifiers.end() )
				iter = m_SupportedModifiers.emplace( uDrmFormat, QuerySampleModifiers( uDrmFormat ) ).first;
			return std::span<const uint64_t>{ iter->second.begin(), iter->second.end() };
		}

		static std::vector<uint64_t> QuerySampleModifiers( uint32_t uDrmFormat )
		{
			std::vector<uint64_t> modifiers;

			VkFormat format = DRMFormatToVulkan( uDrmFormat, false );
			if ( format != VK_FORMAT_UNDEFINED && g_device.supportsModifiers() )
			{
				VkDrmFormatModifierPropertiesListEXT modifierPropList = {
					.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
				};
				VkFormatProperties2 formatProps = {
					.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
					.pNext = &modifierPropList,
				};
				g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

				std::vector<VkDrmFormatModifierPropertiesEXT> props( modifierPropList.drmFormatModifierCount );
				modifierPropList.pDrmFormatModifierProperties = props.data();
				if ( !props.empty() )
					g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

				for ( const VkDrmFormatModifierPropertiesEXT &prop : props )
				{
					if ( prop.drmFormatModifier == DRM_FORMAT_MOD_LINEAR )
						continue;
					if ( prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT )
						modifiers.push_back( prop.drmFormatModifier );
				}
			}

			modifiers.push_back( DRM_FORMAT_MOD_LINEAR );
			return modifiers;
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_EXTERNAL )
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
			if ( nRefresh <= 0 )
				return false;

			int32_t nRefreshmHz = ConvertHztomHz( nRefresh );
			if ( nRefreshmHz == g_nOutputRefresh )
				return true;

			g_nOutputRefresh = nRefreshmHz;
			g_nDynamicRefreshHz = nRefresh;

			m_Connector.UpdateMode( g_nOutputWidth, g_nOutputHeight, (uint32_t)nRefresh );

			wlserver_lock();
			for ( size_t i = 0; i < wlserver.wlr.xwayland_servers.size(); i++ )
				wlserver_set_xwayland_server_mode( i, g_nOutputWidth, g_nOutputHeight, nRefreshmHz );
			wlserver_update_output_manager();
			wlserver_unlock();

			return true;
		}

		virtual void HackUpdatePatchedEdid() override
		{
			if ( !GetCurrentConnector() )
				return;

			if ( cv_hdr_enabled && GetCurrentConnector()->GetHDRInfo().bExposeHDRSupport )
				setenv( "DXVK_HDR", "1", true );
			else
				setenv( "DXVK_HDR", "0", true );

			WritePatchedEdid( GetCurrentConnector()->GetRawEDID(),
				GetCurrentConnector()->GetHDRInfo(), false );
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

        // Per-DRM-format modifier lists for GetSupportedModifiers (filled on first use)
        mutable std::mutex m_ModifierMutex;
        mutable std::map<uint32_t, std::vector<uint64_t>> m_SupportedModifiers;
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
