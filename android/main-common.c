/* Copyright (C) 2011 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
#include <process.h>
#endif

#include <SDL.h>
#include <SDL_syswm.h>

#include "console.h"

#include "android/utils/debug.h"
#include "android/utils/path.h"
#include "android/utils/bufprint.h"
#include "android/utils/dirscanner.h"
#include "android/main-common.h"
#include "android/globals.h"
#include "android/resource.h"
#include "android/user-config.h"
#include "android/qemulator.h"
#include "android/display.h"
#include "android/skin/image.h"
#include "android/skin/trackball.h"
#include "android/skin/keyboard.h"
#include "android/skin/file.h"
#include "android/skin/window.h"



/***********************************************************************/
/***********************************************************************/
/*****                                                             *****/
/*****            U T I L I T Y   R O U T I N E S                  *****/
/*****                                                             *****/
/***********************************************************************/
/***********************************************************************/

#define  D(...)  do {  if (VERBOSE_CHECK(init)) dprint(__VA_ARGS__); } while (0)

/***  CONFIGURATION
 ***/

static AUserConfig*  userConfig;

void
user_config_init( void )
{
    userConfig = auserConfig_new( android_avdInfo );
}

/* only call this function on normal exits, so that ^C doesn't save the configuration */
void
user_config_done( void )
{
    int  win_x, win_y;

    if (!userConfig) {
        D("no user configuration?");
        return;
    }

    SDL_WM_GetPos( &win_x, &win_y );
    auserConfig_setWindowPos(userConfig, win_x, win_y);
    auserConfig_save(userConfig);
}

void
user_config_get_window_pos( int *window_x, int *window_y )
{
    *window_x = *window_y = 10;

    if (userConfig)
        auserConfig_getWindowPos(userConfig, window_x, window_y);
}

unsigned convertBytesToMB( uint64_t  size )
{
    if (size == 0)
        return 0;

    size = (size + ONE_MB-1) >> 20;
    if (size > UINT_MAX)
        size = UINT_MAX;

    return (unsigned) size;
}

uint64_t convertMBToBytes( unsigned  megaBytes )
{
    return ((uint64_t)megaBytes << 20);
}


/***********************************************************************/
/***********************************************************************/
/*****                                                             *****/
/*****            K E Y S E T   R O U T I N E S                    *****/
/*****                                                             *****/
/***********************************************************************/
/***********************************************************************/

#define  KEYSET_FILE    "default.keyset"

SkinKeyset*  android_keyset = NULL;

static int
load_keyset(const char*  path)
{
    if (path_can_read(path)) {
        AConfig*  root = aconfig_node("","");
        if (!aconfig_load_file(root, path)) {
            android_keyset = skin_keyset_new(root);
            if (android_keyset != NULL) {
                D( "keyset loaded from: %s", path);
                return 0;
            }
        }
    }
    return -1;
}

void
parse_keyset(const char*  keyset, AndroidOptions*  opts)
{
    char   kname[MAX_PATH];
    char   temp[MAX_PATH];
    char*  p;
    char*  end;

    /* append .keyset suffix if needed */
    if (strchr(keyset, '.') == NULL) {
        p   =  kname;
        end = p + sizeof(kname);
        p   = bufprint(p, end, "%s.keyset", keyset);
        if (p >= end) {
            derror( "keyset name too long: '%s'\n", keyset);
            exit(1);
        }
        keyset = kname;
    }

    /* look for a the keyset file */
    p   = temp;
    end = p + sizeof(temp);
    p = bufprint_config_file(p, end, keyset);
    if (p < end && load_keyset(temp) == 0)
        return;

    p = temp;
    p = bufprint(p, end, "%s" PATH_SEP "keysets" PATH_SEP "%s", opts->sysdir, keyset);
    if (p < end && load_keyset(temp) == 0)
        return;

    p = temp;
    p = bufprint_app_dir(p, end);
    p = bufprint(p, end, PATH_SEP "keysets" PATH_SEP "%s", keyset);
    if (p < end && load_keyset(temp) == 0)
        return;

    return;
}

void
write_default_keyset( void )
{
    char   path[MAX_PATH];

    bufprint_config_file( path, path+sizeof(path), KEYSET_FILE );

    /* only write if there is no file here */
    if ( !path_exists(path) ) {
        int          fd = open( path, O_WRONLY | O_CREAT, 0666 );
        int          ret;
        const char*  ks = skin_keyset_get_default();


        D( "writing default keyset file to %s", path );

        if (fd < 0) {
            D( "%s: could not create file: %s", __FUNCTION__, strerror(errno) );
            return;
        }
        CHECKED(ret, write(fd, ks, strlen(ks)));
        close(fd);
    }
}



/***********************************************************************/
/***********************************************************************/
/*****                                                             *****/
/*****            S D L   S U P P O R T                            *****/
/*****                                                             *****/
/***********************************************************************/
/***********************************************************************/

void *readpng(const unsigned char*  base, size_t  size, unsigned *_width, unsigned *_height);

#ifdef CONFIG_DARWIN
#  define  ANDROID_ICON_PNG  "android_icon_256.png"
#else
#  define  ANDROID_ICON_PNG  "android_icon_16.png"
#endif

static void
sdl_set_window_icon( void )
{
    static int  window_icon_set;

    if (!window_icon_set)
    {
#ifdef _WIN32
        HANDLE         handle = GetModuleHandle( NULL );
        HICON          icon   = LoadIcon( handle, MAKEINTRESOURCE(1) );
        SDL_SysWMinfo  wminfo;

        SDL_GetWMInfo(&wminfo);

        SetClassLong( wminfo.window, GCL_HICON, (LONG)icon );
#else  /* !_WIN32 */
        unsigned              icon_w, icon_h;
        size_t                icon_bytes;
        const unsigned char*  icon_data;
        void*                 icon_pixels;

        window_icon_set = 1;

        icon_data = android_icon_find( ANDROID_ICON_PNG, &icon_bytes );
        if ( !icon_data )
            return;

        icon_pixels = readpng( icon_data, icon_bytes, &icon_w, &icon_h );
        if ( !icon_pixels )
            return;

       /* the data is loaded into memory as RGBA bytes by libpng. we want to manage
        * the values as 32-bit ARGB pixels, so swap the bytes accordingly depending
        * on our CPU endianess
        */
        {
            unsigned*  d     = icon_pixels;
            unsigned*  d_end = d + icon_w*icon_h;

            for ( ; d < d_end; d++ ) {
                unsigned  pix = d[0];
#if HOST_WORDS_BIGENDIAN
                /* R,G,B,A read as RGBA => ARGB */
                pix = ((pix >> 8) & 0xffffff) | (pix << 24);
#else
                /* R,G,B,A read as ABGR => ARGB */
                pix = (pix & 0xff00ff00) | ((pix >> 16) & 0xff) | ((pix & 0xff) << 16);
#endif
                d[0] = pix;
            }
        }

        SDL_Surface* icon = sdl_surface_from_argb32( icon_pixels, icon_w, icon_h );
        if (icon != NULL) {
            SDL_WM_SetIcon(icon, NULL);
            SDL_FreeSurface(icon);
            free( icon_pixels );
        }
#endif  /* !_WIN32 */
    }
}

/***********************************************************************/
/***********************************************************************/
/*****                                                             *****/
/*****            S K I N   S U P P O R T                          *****/
/*****                                                             *****/
/***********************************************************************/
/***********************************************************************/

const char*  skin_network_speed = NULL;
const char*  skin_network_delay = NULL;


static void sdl_at_exit(void)
{
    user_config_done();
    qemulator_done(qemulator_get());
    SDL_Quit();
}


void sdl_display_init(DisplayState *ds, int full_screen, int  no_frame)
{
    QEmulator*    emulator = qemulator_get();
    SkinDisplay*  disp     = skin_layout_get_display(emulator->layout);
    int           width, height;
    char          buf[128];

    if (disp->rotation & 1) {
        width  = disp->rect.size.h;
        height = disp->rect.size.w;
    } else {
        width  = disp->rect.size.w;
        height = disp->rect.size.h;
    }

    snprintf(buf, sizeof buf, "width=%d,height=%d", width, height);
#if !defined(CONFIG_STANDALONE_UI) && !defined(CONFIG_STANDALONE_CORE)
    android_display_init(ds, qframebuffer_fifo_get());
#endif
}

/* list of skin aliases */
static const struct {
    const char*  name;
    const char*  alias;
} skin_aliases[] = {
    { "QVGA-L", "320x240" },
    { "QVGA-P", "240x320" },
    { "HVGA-L", "480x320" },
    { "HVGA-P", "320x480" },
    { "QVGA", "320x240" },
    { "HVGA", "320x480" },
    { NULL, NULL }
};

/* this is used by hw/events_device.c to send the charmap name to the system */
const char*    android_skin_keycharmap = NULL;

void
parse_skin_files(const char*      skinDirPath,
                 const char*      skinName,
                 AndroidOptions*  opts,
                 AndroidHwConfig* hwConfig,
                 AConfig*        *skinConfig,
                 char*           *skinPath)
{
    char      tmp[1024];
    AConfig*  root;
    const char* path = NULL;
    AConfig*  n;

    root = aconfig_node("", "");

    if (skinName == NULL)
        goto DEFAULT_SKIN;

    /* Support skin aliases like QVGA-H QVGA-P, etc...
       But first we check if it's a directory that exist before applying
       the alias */
    int  checkAlias = 1;

    if (skinDirPath != NULL) {
        bufprint(tmp, tmp+sizeof(tmp), "%s/%s", skinDirPath, skinName);
        if (path_exists(tmp)) {
            checkAlias = 0;
        } else {
            D("there is no '%s' skin in '%s'", skinName, skinDirPath);
        }
    }

    if (checkAlias) {
        int  nn;

        for (nn = 0; ; nn++ ) {
            const char*  skin_name  = skin_aliases[nn].name;
            const char*  skin_alias = skin_aliases[nn].alias;

            if (!skin_name)
                break;

            if (!strcasecmp( skin_name, skinName )) {
                D("skin name '%s' aliased to '%s'", skinName, skin_alias);
                skinName = skin_alias;
                break;
            }
        }
    }

    /* Magically support skins like "320x240" or "320x240x16" */
    if(isdigit(skinName[0])) {
        char *x = strchr(skinName, 'x');
        if(x && isdigit(x[1])) {
            int width = atoi(skinName);
            int height = atoi(x+1);
            int bpp   = 16;
            char* y = strchr(x+1, 'x');
            if (y && isdigit(y[1])) {
                bpp = atoi(y+1);
            }
            snprintf(tmp, sizeof tmp,
                        "display {\n  width %d\n  height %d\n bpp %d}\n",
                        width, height,bpp);
            aconfig_load(root, strdup(tmp));
            path = ":";
            D("found magic skin width=%d height=%d bpp=%d\n", width, height, bpp);
            goto FOUND_SKIN;
        }
    }

    if (skinDirPath == NULL) {
        derror("unknown skin name '%s'", skinName);
        exit(1);
    }

    snprintf(tmp, sizeof tmp, "%s/%s/layout", skinDirPath, skinName);
    D("trying to load skin file '%s'", tmp);

    if(aconfig_load_file(root, tmp) < 0) {
        dwarning("could not load skin file '%s', using built-in one\n",
                 tmp);
        goto DEFAULT_SKIN;
    }

    snprintf(tmp, sizeof tmp, "%s/%s/", skinDirPath, skinName);
    path = tmp;
    goto FOUND_SKIN;

FOUND_SKIN:
    /* the default network speed and latency can now be specified by the device skin */
    n = aconfig_find(root, "network");
    if (n != NULL) {
        skin_network_speed = aconfig_str(n, "speed", 0);
        skin_network_delay = aconfig_str(n, "delay", 0);
    }

    /* extract framebuffer information from the skin.
     *
     * for version 1 of the skin format, they are in the top-level
     * 'display' element.
     *
     * for version 2 of the skin format, they are under parts.device.display
     */
    n = aconfig_find(root, "display");
    if (n == NULL) {
        n = aconfig_find(root, "parts");
        if (n != NULL) {
            n = aconfig_find(root, "device");
            if (n != NULL) {
                n = aconfig_find(root, "display");
            }
        }
    }

    if (n != NULL) {
        int  width  = aconfig_int(n, "width", hwConfig->hw_lcd_width);
        int  height = aconfig_int(n, "height", hwConfig->hw_lcd_height);
        int  depth  = aconfig_int(n, "bpp", hwConfig->hw_lcd_depth);

        if (width > 0 && height > 0) {
            /* The emulated framebuffer wants sizes that are multiples of 4 */
            if (((width|height) & 3) != 0) {
                width  = (width+3) & ~3;
                height = (height+3) & ~3;
                D("adjusting LCD dimensions to (%dx%dx)", width, height);
            }

            /* only depth values of 16 and 32 are correct. 16 is the default. */
            if (depth != 32 && depth != 16) {
                depth = 16;
                D("adjusting LCD bit depth to %d", depth);
            }

            hwConfig->hw_lcd_width  = width;
            hwConfig->hw_lcd_height = height;
            hwConfig->hw_lcd_depth  = depth;
        }
        else {
            D("ignoring invalid skin LCD dimensions (%dx%dx%d)",
              width, height, depth);
        }
    }

    *skinConfig = root;
    *skinPath   = strdup(path);
    return;

DEFAULT_SKIN:
    {
        const unsigned char*  layout_base;
        size_t                layout_size;
        char*                 base;

        skinName = "<builtin>";

        layout_base = android_resource_find( "layout", &layout_size );
        if (layout_base == NULL) {
            fprintf(stderr, "Couldn't load builtin skin\n");
            exit(1);
        }
        base = malloc( layout_size+1 );
        memcpy( base, layout_base, layout_size );
        base[layout_size] = 0;

        D("parsing built-in skin layout file (%d bytes)", (int)layout_size);
        aconfig_load(root, base);
        path = ":";
    }
    goto FOUND_SKIN;
}


void
init_sdl_ui(AConfig*         skinConfig,
            const char*      skinPath,
            AndroidOptions*  opts)
{
    int  win_x, win_y, flags;

    signal(SIGINT, SIG_DFL);
#ifndef _WIN32
    signal(SIGQUIT, SIG_DFL);
#endif

    /* we're not a game, so allow the screensaver to run */
    putenv("SDL_VIDEO_ALLOW_SCREENSAVER=1");

    flags = SDL_INIT_NOPARACHUTE;
    if (!opts->no_window)
        flags |= SDL_INIT_VIDEO;

    if(SDL_Init(flags)){
        fprintf(stderr, "SDL init failure, reason is: %s\n", SDL_GetError() );
        exit(1);
    }

    if (!opts->no_window) {
        SDL_EnableUNICODE(!opts->raw_keys);
        SDL_EnableKeyRepeat(0,0);

        sdl_set_window_icon();
    }
    else
    {
#ifndef _WIN32
       /* prevent SIGTTIN and SIGTTOUT from stopping us. this is necessary to be
        * able to run the emulator in the background (e.g. "emulator &").
        * despite the fact that the emulator should not grab input or try to
        * write to the output in normal cases, we're stopped on some systems
        * (e.g. OS X)
        */
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
#endif
    }
    atexit(sdl_at_exit);

    user_config_get_window_pos(&win_x, &win_y);

    if ( qemulator_init(qemulator_get(), skinConfig, skinPath, win_x, win_y, opts) < 0 ) {
        fprintf(stderr, "### Error: could not load emulator skin from '%s'\n", skinPath);
        exit(1);
    }

    android_skin_keycharmap = skin_keyboard_charmap_name(qemulator_get()->keyboard);

    /* add an onion overlay image if needed */
    if (opts->onion) {
        SkinImage*  onion = skin_image_find_simple( opts->onion );
        int         alpha, rotate;

        if ( opts->onion_alpha && 1 == sscanf( opts->onion_alpha, "%d", &alpha ) ) {
            alpha = (256*alpha)/100;
        } else
            alpha = 128;

        if ( opts->onion_rotation && 1 == sscanf( opts->onion_rotation, "%d", &rotate ) ) {
            rotate &= 3;
        } else
            rotate = SKIN_ROTATION_0;

        qemulator_get()->onion          = onion;
        qemulator_get()->onion_alpha    = alpha;
        qemulator_get()->onion_rotation = rotate;
    }
}

/* this function is used to perform auto-detection of the
 * system directory in the case of a SDK installation.
 *
 * we want to deal with several historical usages, hence
 * the slightly complicated logic.
 *
 * NOTE: the function returns the path to the directory
 *       containing 'fileName'. this is *not* the full
 *       path to 'fileName'.
 */
static char*
_getSdkImagePath( const char*  fileName )
{
    char   temp[MAX_PATH];
    char*  p   = temp;
    char*  end = p + sizeof(temp);
    char*  q;
    char*  app;

    static const char* const  searchPaths[] = {
        "",                                  /* program's directory */
        "/lib/images",                       /* this is for SDK 1.0 */
        "/../platforms/android-1.1/images",  /* this is for SDK 1.1 */
        NULL
    };

    app = bufprint_app_dir(temp, end);
    if (app >= end)
        return NULL;

    do {
        int  nn;

        /* first search a few well-known paths */
        for (nn = 0; searchPaths[nn] != NULL; nn++) {
            p = bufprint(app, end, "%s", searchPaths[nn]);
            q = bufprint(p, end, "/%s", fileName);
            if (q < end && path_exists(temp)) {
                *p = 0;
                goto FOUND_IT;
            }
        }

        /* hmmm. let's assume that we are in a post-1.1 SDK
         * scan ../platforms if it exists
         */
        p = bufprint(app, end, "/../platforms");
        if (p < end) {
            DirScanner*  scanner = dirScanner_new(temp);
            if (scanner != NULL) {
                int          found = 0;
                const char*  subdir;

                for (;;) {
                    subdir = dirScanner_next(scanner);
                    if (!subdir) break;

                    q = bufprint(p, end, "/%s/images/%s", subdir, fileName);
                    if (q >= end || !path_exists(temp))
                        continue;

                    found = 1;
                    p = bufprint(p, end, "/%s/images", subdir);
                    break;
                }
                dirScanner_free(scanner);
                if (found)
                    break;
            }
        }

        /* I'm out of ideas */
        return NULL;

    } while (0);

FOUND_IT:
    //D("image auto-detection: %s/%s", temp, fileName);
    return android_strdup(temp);
}

static char*
_getSdkImage( const char*  path, const char*  file )
{
    char  temp[MAX_PATH];
    char  *p = temp, *end = p + sizeof(temp);

    p = bufprint(temp, end, "%s/%s", path, file);
    if (p >= end || !path_exists(temp))
        return NULL;

    return android_strdup(temp);
}

static char*
_getSdkSystemImage( const char*  path, const char*  optionName, const char*  file )
{
    char*  image = _getSdkImage(path, file);

    if (image == NULL) {
        derror("Your system directory is missing the '%s' image file.\n"
               "Please specify one with the '%s <filepath>' option",
               file, optionName);
        exit(2);
    }
    return image;
}

static void
_forceAvdImagePath( AvdImageType  imageType,
                   const char*   path,
                   const char*   description,
                   int           required )
{
    if (path == NULL)
        return;

    if (required && !path_exists(path)) {
        derror("Cannot find %s image file: %s", description, path);
        exit(1);
    }
    android_avdParams->forcePaths[imageType] = path;
}

AvdInfo* createAVD(AndroidOptions* opts, int* inAndroidBuild)
{
    AvdInfo* ret = NULL;
    char   tmp[MAX_PATH];
    char*  tmpend = tmp + sizeof(tmp);
    char*       android_build_root = NULL;
    char*       android_build_out  = NULL;

    /* setup the virtual device parameters from our options
     */
    if (opts->no_cache) {
        android_avdParams->flags |= AVDINFO_NO_CACHE;
    }
    if (opts->wipe_data) {
        android_avdParams->flags |= AVDINFO_WIPE_DATA | AVDINFO_WIPE_CACHE;
    }
#if CONFIG_ANDROID_SNAPSHOTS
    if (opts->no_snapstorage) {
        android_avdParams->flags |= AVDINFO_NO_SNAPSHOTS;
    }
#endif

    /* legacy support: we used to use -system <dir> and -image <file>
     * instead of -sysdir <dir> and -system <file>, so handle this by checking
     * whether the options point to directories or files.
     */
    if (opts->image != NULL) {
        if (opts->system != NULL) {
            if (opts->sysdir != NULL) {
                derror( "You can't use -sysdir, -system and -image at the same time.\n"
                        "You should probably use '-sysdir <path> -system <file>'.\n" );
                exit(2);
            }
        }
        dwarning( "Please note that -image is obsolete and that -system is now used to point\n"
                  "to the system image. Next time, try using '-sysdir <path> -system <file>' instead.\n" );
        opts->sysdir = opts->system;
        opts->system = opts->image;
        opts->image  = NULL;
    }
    else if (opts->system != NULL && path_is_dir(opts->system)) {
        if (opts->sysdir != NULL) {
            derror( "Option -system should now be followed by a file path, not a directory one.\n"
                    "Please use '-sysdir <path>' to point to the system directory.\n" );
            exit(1);
        }
        dwarning( "Please note that the -system option should now be used to point to the initial\n"
                  "system image (like the obsolete -image option). To point to the system directory\n"
                  "please now use '-sysdir <path>' instead.\n" );

        opts->sysdir = opts->system;
        opts->system = NULL;
    }

    /* If no AVD name was given, try to find the top of the
     * Android build tree
     */
    if (opts->avd == NULL) {
        do {
            char*  out = getenv("ANDROID_PRODUCT_OUT");

            if (out == NULL || out[0] == 0)
                break;

            if (!path_exists(out)) {
                derror("Can't access ANDROID_PRODUCT_OUT as '%s'\n"
                    "You need to build the Android system before launching the emulator",
                    out);
                exit(2);
            }

            android_build_root = path_parent( out, 4 );
            if (android_build_root == NULL || !path_exists(android_build_root)) {
                derror("Can't find the Android build root from '%s'\n"
                    "Please check the definition of the ANDROID_PRODUCT_OUT variable.\n"
                    "It should point to your product-specific build output directory.\n",
                    out );
                exit(2);
            }
            android_build_out = out;
            D( "found Android build root: %s", android_build_root );
            D( "found Android build out:  %s", android_build_out );
        } while (0);
    }
    /* if no virtual device name is given, and we're not in the
     * Android build system, we'll need to perform some auto-detection
     * magic :-)
     */
    if (opts->avd == NULL && !android_build_out)
    {
        char   dataDirIsSystem = 0;

        if (!opts->sysdir) {
            opts->sysdir = _getSdkImagePath("system.img");
            if (!opts->sysdir) {
                derror(
                "You did not specify a virtual device name, and the system\n"
                "directory could not be found.\n\n"
                "If you are an Android SDK user, please use '@<name>' or '-avd <name>'\n"
                "to start a given virtual device (see -help-avd for details).\n\n"

                "Otherwise, follow the instructions in -help-disk-images to start the emulator\n"
                );
                exit(2);
            }
            D("autoconfig: -sysdir %s", opts->sysdir);
        }

        if (!opts->system) {
            opts->system = _getSdkSystemImage(opts->sysdir, "-image", "system.img");
            D("autoconfig: -image %s", opts->image);
        }

        if (!opts->kernel) {
            opts->kernel = _getSdkSystemImage(opts->sysdir, "-kernel", "kernel-qemu");
            D("autoconfig: -kernel %s", opts->kernel);
        }

        if (!opts->ramdisk) {
            opts->ramdisk = _getSdkSystemImage(opts->sysdir, "-ramdisk", "ramdisk.img");
            D("autoconfig: -ramdisk %s", opts->ramdisk);
        }

        /* if no data directory is specified, use the system directory */
        if (!opts->datadir) {
            opts->datadir   = android_strdup(opts->sysdir);
            dataDirIsSystem = 1;
            D("autoconfig: -datadir %s", opts->sysdir);
        }

        if (!opts->data) {
            /* check for userdata-qemu.img in the data directory */
            bufprint(tmp, tmpend, "%s/userdata-qemu.img", opts->datadir);
            if (!path_exists(tmp)) {
                derror(
                "You did not provide the name of an Android Virtual Device\n"
                "with the '-avd <name>' option. Read -help-avd for more information.\n\n"

                "If you *really* want to *NOT* run an AVD, consider using '-data <file>'\n"
                "to specify a data partition image file (I hope you know what you're doing).\n"
                );
                exit(2);
            }

            opts->data = android_strdup(tmp);
            D("autoconfig: -data %s", opts->data);
        }

        if (!opts->sdcard && opts->datadir) {
            bufprint(tmp, tmpend, "%s/sdcard.img", opts->datadir);
            if (path_exists(tmp)) {
                opts->sdcard = android_strdup(tmp);
                D("autoconfig: -sdcard %s", opts->sdcard);
            }
        }

#if CONFIG_ANDROID_SNAPSHOTS
        if (!opts->snapstorage && opts->datadir) {
            bufprint(tmp, tmpend, "%s/snapshots.img", opts->datadir);
            if (path_exists(tmp)) {
                opts->snapstorage = android_strdup(tmp);
                D("autoconfig: -snapstorage %s", opts->snapstorage);
            }
        }
#endif // CONFIG_ANDROID_SNAPSHOTS
    }

    /* if certain options are set, we can force the path of
        * certain kernel/disk image files
        */
    _forceAvdImagePath(AVD_IMAGE_KERNEL,     opts->kernel,      "kernel", 1);
    _forceAvdImagePath(AVD_IMAGE_INITSYSTEM, opts->system,      "system", 1);
    _forceAvdImagePath(AVD_IMAGE_RAMDISK,    opts->ramdisk,     "ramdisk", 1);
    _forceAvdImagePath(AVD_IMAGE_USERDATA,   opts->data,        "user data", 0);
    _forceAvdImagePath(AVD_IMAGE_CACHE,      opts->cache,       "cache", 0);
    _forceAvdImagePath(AVD_IMAGE_SDCARD,     opts->sdcard,      "SD Card", 0);
#if CONFIG_ANDROID_SNAPSHOTS
    _forceAvdImagePath(AVD_IMAGE_SNAPSHOTS,  opts->snapstorage, "snapshots", 0);
#endif

    /* we don't accept -skindir without -skin now
     * to simplify the autoconfig stuff with virtual devices
     */
    if (opts->no_skin) {
        opts->skin    = "320x480";
        opts->skindir = NULL;
    }

    if (opts->skindir) {
        if (!opts->skin) {
            derror( "the -skindir <path> option requires a -skin <name> option");
            exit(1);
        }
    }
    android_avdParams->skinName     = opts->skin;
    android_avdParams->skinRootPath = opts->skindir;

    /* setup the virtual device differently depending on whether
     * we are in the Android build system or not
     */
    if (opts->avd != NULL)
    {
        ret = avdInfo_new( opts->avd, android_avdParams );
        if (ret == NULL) {
            /* an error message has already been printed */
            dprint("could not find virtual device named '%s'", opts->avd);
            exit(1);
        }
    }
    else
    {
        if (!android_build_out) {
            android_build_out = android_build_root = opts->sysdir;
        }
        ret = avdInfo_newForAndroidBuild(
                            android_build_root,
                            android_build_out,
                            android_avdParams );

        if(ret == NULL) {
            D("could not start virtual device\n");
            exit(1);
        }
    }

    if (android_build_out) {
        *inAndroidBuild = 1;
    } else {
        *inAndroidBuild = 0;
    }

    return ret;
}

static int
_is_valid_hw_disk_path(const char* path)
{
    if (path == NULL || '\0' == *path || !strcmp(path, "<init>")) {
        return 0;
    }
    return 1;
}

static int
_update_hwconfig_path(char** path,  AvdInfo* avd, int image_type)
{
    if (!_is_valid_hw_disk_path(*path)) {
        *path = android_strdup(avdInfo_getImageFile(avd, image_type));
        if (!_is_valid_hw_disk_path(*path)) {
            return -1;
        }
    }
    return 0;
}

static uint64_t
_adjustPartitionSize( const char*  description,
                      uint64_t     imageBytes,
                      uint64_t     defaultBytes,
                      int          inAndroidBuild )
{
    char      temp[64];
    unsigned  imageMB;
    unsigned  defaultMB;

    if (imageBytes <= defaultBytes)
        return defaultBytes;

    imageMB   = convertBytesToMB(imageBytes);
    defaultMB = convertBytesToMB(defaultBytes);

    if (imageMB > defaultMB) {
        snprintf(temp, sizeof temp, "(%d MB > %d MB)", imageMB, defaultMB);
    } else {
        snprintf(temp, sizeof temp, "(%lld bytes > %lld bytes)", imageBytes, defaultBytes);
    }

    if (inAndroidBuild) {
        dwarning("%s partition size adjusted to match image file %s\n", description, temp);
    }

    return convertMBToBytes(imageMB);
}

void
updateHwConfigFromAVD(AndroidHwConfig* hwConfig,
                      AvdInfo* avd,
                      AndroidOptions* opts,
                      int inAndroidBuild)
{
    unsigned  defaultPartitionSize = convertMBToBytes(66);

    if (_update_hwconfig_path(&hwConfig->disk_kernel_path, avd, AVD_IMAGE_KERNEL)) {
        dprint("kernel image path %s is invalid", hwConfig->disk_kernel_path);
        exit(1);
    }

    if (_update_hwconfig_path(&hwConfig->disk_ramDisk_path, avd, AVD_IMAGE_RAMDISK)) {
        dprint("ramdisk image path %s is invalid", hwConfig->disk_ramDisk_path);
        exit(1);
    }

    if (_update_hwconfig_path(&hwConfig->disk_systemPartition_initPath, avd, AVD_IMAGE_INITSYSTEM)) {
        dprint("system path %s is invalid", hwConfig->disk_systemPartition_initPath);
        exit(1);
    }

    _update_hwconfig_path(&hwConfig->disk_dataPartition_path, avd, AVD_IMAGE_USERDATA);
    _update_hwconfig_path(&hwConfig->disk_systemPartition_path, avd, AVD_IMAGE_USERSYSTEM);
    _update_hwconfig_path(&hwConfig->disk_dataPartition_path, avd, AVD_IMAGE_INITDATA);
    _update_hwconfig_path(&hwConfig->disk_sdCard_path, avd, AVD_IMAGE_SDCARD);
    _update_hwconfig_path(&hwConfig->disk_cachePartition_path, avd, AVD_IMAGE_CACHE);
#if CONFIG_ANDROID_SNAPSHOTS
    _update_hwconfig_path(&hwConfig->disk_snapshots_path, avd, AVD_IMAGE_SNAPSHOTS);
#endif  // CONFIG_ANDROID_SNAPSHOTS

    if (opts->partition_size) {
        char*  end;
        long   sizeMB = strtol(opts->partition_size, &end, 0);
        long   minSizeMB = 10;
        long   maxSizeMB = LONG_MAX / ONE_MB;

        if (sizeMB < 0 || *end != 0) {
            derror( "-partition-size must be followed by a positive integer" );
            exit(1);
        }
        if (sizeMB < minSizeMB || sizeMB > maxSizeMB) {
            derror( "partition-size (%d) must be between %dMB and %dMB",
                    sizeMB, minSizeMB, maxSizeMB );
            exit(1);
        }
        defaultPartitionSize = sizeMB * ONE_MB;
    }

    /* Check the size of the system partition image.
     * If we have an AVD, it must be smaller than
     * the disk.systemPartition.size hardware property.
     *
     * Otherwise, we need to adjust the systemPartitionSize
     * automatically, and print a warning.
     *
     */
    {
        uint64_t   systemBytes  = avdInfo_getImageFileSize(avd, AVD_IMAGE_INITSYSTEM);
        uint64_t   defaultBytes = defaultPartitionSize;

        if (defaultBytes == 0 || opts->partition_size)
            defaultBytes = defaultPartitionSize;
        hwConfig->disk_systemPartition_size =
            _adjustPartitionSize("system", systemBytes, defaultBytes, inAndroidBuild);
    }

    /* Check the size of the /data partition. The only interesting cases here are:
     * - when the USERDATA image already exists and is larger than the default
     * - when we're wiping data and the INITDATA is larger than the default.
     */

    {
        const char*  dataPath     = avdInfo_getImageFile(avd, AVD_IMAGE_USERDATA);
        uint64_t     defaultBytes = defaultPartitionSize;

        if (defaultBytes == 0 || opts->partition_size)
            defaultBytes = defaultPartitionSize;

        if (dataPath == NULL || !path_exists(dataPath) || opts->wipe_data) {
            dataPath = avdInfo_getImageFile(avd, AVD_IMAGE_INITDATA);
        }
        if (dataPath == NULL || !path_exists(dataPath)) {
            hwConfig->disk_dataPartition_size = defaultBytes;
        }
        else {
            uint64_t  dataBytes;
            path_get_size(dataPath, &dataBytes);

            hwConfig->disk_dataPartition_size =
                _adjustPartitionSize("data", dataBytes, defaultBytes, inAndroidBuild);
        }
    }
}