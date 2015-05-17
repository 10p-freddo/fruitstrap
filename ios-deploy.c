//TODO: don't copy/mount DeveloperDiskImage.dmg if it's already done - Xcode checks this somehow

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <pwd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "MobileDevice.h"
#include "errors.h"

#define APP_VERSION    "1.7.1"
#define PREP_CMDS_PATH "/tmp/fruitstrap-lldb-prep-cmds-"
#define LLDB_SHELL "lldb -s " PREP_CMDS_PATH
/*
 * Startup script passed to lldb.
 * To see how xcode interacts with lldb, put this into .lldbinit:
 * log enable -v -f /Users/vargaz/lldb.log lldb all
 * log enable -v -f /Users/vargaz/gdb-remote.log gdb-remote all
 */
#define LLDB_PREP_CMDS CFSTR("\
    platform select remote-ios --sysroot {symbols_path}\n\
    target create \"{disk_app}\"\n\
    script fruitstrap_device_app=\"{device_app}\"\n\
    script fruitstrap_connect_url=\"connect://127.0.0.1:{device_port}\"\n\
    command script import \"{python_file_path}\"\n\
    command script add -f {python_command}.connect_command connect\n\
    command script add -s asynchronous -f {python_command}.run_command run\n\
    command script add -s asynchronous -f {python_command}.autoexit_command autoexit\n\
    command script add -s asynchronous -f {python_command}.safequit_command safequit\n\
    connect\n\
")

const char* lldb_prep_no_cmds = "";

const char* lldb_prep_interactive_cmds = "\
    run\n\
";

const char* lldb_prep_noninteractive_justlaunch_cmds = "\
    run\n\
    safequit\n\
";

const char* lldb_prep_noninteractive_cmds = "\
    run\n\
    autoexit\n\
";

/*
 * Some things do not seem to work when using the normal commands like process connect/launch, so we invoke them
 * through the python interface. Also, Launch () doesn't seem to work when ran from init_module (), so we add
 * a command which can be used by the user to run it.
 */
#define LLDB_FRUITSTRAP_MODULE CFSTR("\
import lldb\n\
import os\n\
import sys\n\
import shlex\n\
\n\
def connect_command(debugger, command, result, internal_dict):\n\
    # These two are passed in by the script which loads us\n\
    connect_url = internal_dict['fruitstrap_connect_url']\n\
    error = lldb.SBError()\n\
\n\
    process = lldb.target.ConnectRemote(lldb.target.GetDebugger().GetListener(), connect_url, None, error)\n\
\n\
    # Wait for connection to succeed\n\
    listener = lldb.target.GetDebugger().GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged)\n\
    events = []\n\
    state = (process.GetState() or lldb.eStateInvalid)\n\
    while state != lldb.eStateConnected:\n\
        event = lldb.SBEvent()\n\
        if listener.WaitForEvent(1, event):\n\
            state = process.GetStateFromEvent(event)\n\
            events.append(event)\n\
        else:\n\
            state = lldb.eStateInvalid\n\
\n\
    # Add events back to queue, otherwise lldb freezes\n\
    for event in events:\n\
        listener.AddEvent(event)\n\
\n\
def run_command(debugger, command, result, internal_dict):\n\
    device_app = internal_dict['fruitstrap_device_app']\n\
    args = command.split('--',1)\n\
    error = lldb.SBError()\n\
    lldb.target.modules[0].SetPlatformFileSpec(lldb.SBFileSpec(device_app))\n\
    lldb.target.Launch(lldb.SBLaunchInfo(shlex.split(args[1] and args[1] or '{args}')), error)\n\
    lockedstr = ': Locked'\n\
    if lockedstr in str(error):\n\
       print('\\nDevice Locked\\n')\n\
       os._exit(254)\n\
    else:\n\
       print(str(error))\n\
\n\
def safequit_command(debugger, command, result, internal_dict):\n\
    process = lldb.target.process\n\
    listener = debugger.GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged | lldb.SBProcess.eBroadcastBitSTDOUT | lldb.SBProcess.eBroadcastBitSTDERR)\n\
    event = lldb.SBEvent()\n\
    while True:\n\
        if listener.WaitForEvent(1, event) and lldb.SBProcess.EventIsProcessEvent(event):\n\
            state = lldb.SBProcess.GetStateFromEvent(event)\n\
        else:\n\
            state = process.GetState()\n\
\n\
        if state == lldb.eStateRunning:\n\
            process.Detach()\n\
            os._exit(0)\n\
        elif state > lldb.eStateRunning:\n\
            os._exit(state)\n\
\n\
def autoexit_command(debugger, command, result, internal_dict):\n\
    process = lldb.target.process\n\
    listener = debugger.GetListener()\n\
    listener.StartListeningForEvents(process.GetBroadcaster(), lldb.SBProcess.eBroadcastBitStateChanged | lldb.SBProcess.eBroadcastBitSTDOUT | lldb.SBProcess.eBroadcastBitSTDERR)\n\
    event = lldb.SBEvent()\n\
    while True:\n\
        if listener.WaitForEvent(1, event) and lldb.SBProcess.EventIsProcessEvent(event):\n\
            state = lldb.SBProcess.GetStateFromEvent(event)\n\
        else:\n\
            state = process.GetState()\n\
\n\
        if state == lldb.eStateExited:\n\
            os._exit(process.GetExitStatus())\n\
        elif state == lldb.eStateStopped:\n\
            debugger.HandleCommand('bt')\n\
            os._exit({exitcode_app_crash})\n\
\n\
        stdout = process.GetSTDOUT(1024)\n\
        while stdout:\n\
            sys.stdout.write(stdout)\n\
            stdout = process.GetSTDOUT(1024)\n\
\n\
        stderr = process.GetSTDERR(1024)\n\
        while stderr:\n\
            sys.stdout.write(stderr)\n\
            stderr = process.GetSTDERR(1024)\n\
")

typedef struct am_device * AMDeviceRef;
mach_error_t AMDeviceSecureStartService(struct am_device *device, CFStringRef service_name, unsigned int *unknown, service_conn_t *handle);
int AMDeviceSecureTransferPath(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceSecureInstallApplication(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceMountImage(AMDeviceRef device, CFStringRef image, CFDictionaryRef options, void *callback, int cbarg);
mach_error_t AMDeviceLookupApplications(AMDeviceRef device, CFDictionaryRef options, CFDictionaryRef *result);
int AMDeviceGetInterfaceType(struct am_device *device);

bool found_device = false, debug = false, verbose = false, unbuffered = false, nostart = false, detect_only = false, install = true, uninstall = false;
bool command_only = false;
char *command = NULL;
char *target_filename = NULL;
char *upload_pathname = NULL;
char *bundle_id = NULL;
bool interactive = true;
bool justlaunch = false;
char *app_path = NULL;
char *device_id = NULL;
char *args = NULL;
char *list_root = NULL;
int timeout = 0;
int port = 0;	// 0 means "dynamically assigned"
CFStringRef last_path = NULL;
service_conn_t gdbfd;
pid_t parent = 0;
// PID of child process running lldb
pid_t child = 0;
// Signal sent from child to parent process when LLDB finishes.
const int SIGLLDB = SIGUSR1;
AMDeviceRef best_device_match = NULL;

// Error codes we report on different failures, so scripts can distinguish between user app exit
// codes and our exit codes. For non app errors we use codes in reserved 128-255 range.
const int exitcode_error = 253;
const int exitcode_app_crash = 254;

// Checks for MobileDevice.framework errors, tries to print them and exits.
#define check_error(call)                                                       \
    do {                                                                        \
        unsigned int err = (unsigned int)call;                                  \
        if (err != 0)                                                           \
        {                                                                       \
            const char* msg = get_error_message(err);                           \
            on_error("Error 0x%x: %s " #call, err, msg ? msg : "unknown.");     \
        }                                                                       \
    } while (false);

// Print error message and exit
void on_error(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ !! ] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    exit(exitcode_error);
}

// Print error message getting last errno and exit
void on_sys_error(const char* fmt, ...) {
    const char* errstr = strerror(errno);

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 256, fmt, args);
    on_error("%s: %s", buf, errstr);
    va_end(args);
}

Boolean path_exists(CFTypeRef path) {
    if (CFGetTypeID(path) == CFStringGetTypeID()) {
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
        Boolean result = CFURLResourceIsReachable(url, NULL);
        CFRelease(url);
        return result;
    } else if (CFGetTypeID(path) == CFURLGetTypeID()) {
        return CFURLResourceIsReachable(path, NULL);
    } else {
        return false;
    }
}

CFStringRef find_path(CFStringRef rootPath, CFStringRef namePattern, CFStringRef expression) {
    FILE *fpipe = NULL;
    CFStringRef quotedRootPath = rootPath;
    CFStringRef cf_command;
    CFRange slashLocation;

    if (CFStringGetCharacterAtIndex(rootPath, 0) != '`') {
        quotedRootPath = CFStringCreateWithFormat(NULL, NULL, CFSTR("'%@'"), rootPath);
    }

    slashLocation = CFStringFind(namePattern, CFSTR("/"), 0);
    if (slashLocation.location == kCFNotFound) {
        cf_command = CFStringCreateWithFormat(NULL, NULL, CFSTR("find %@ -name '%@' %@ 2>/dev/null | sort | tail -n 1"), quotedRootPath, namePattern, expression);
    } else {
        cf_command = CFStringCreateWithFormat(NULL, NULL, CFSTR("find %@ -path '%@' %@ 2>/dev/null | sort | tail -n 1"), quotedRootPath, namePattern, expression);
    }

    if (quotedRootPath != rootPath) {
        CFRelease(quotedRootPath);
    }

    char command[1024] = { '\0' };
    CFStringGetCString(cf_command, command, sizeof(command), kCFStringEncodingUTF8);
    CFRelease(cf_command);

    if (!(fpipe = (FILE *)popen(command, "r")))
        on_sys_error("Error encountered while opening pipe");

    char buffer[256] = { '\0' };

    fgets(buffer, sizeof(buffer), fpipe);
    pclose(fpipe);

    strtok(buffer, "\n");
    return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

CFStringRef copy_long_shot_disk_image_path() {
    return find_path(CFSTR("`xcode-select --print-path`"), CFSTR("DeveloperDiskImage.dmg"), CFSTR(""));
}

CFStringRef copy_xcode_dev_path() {
    static char xcode_dev_path[256] = { '\0' };
    if (strlen(xcode_dev_path) == 0) {
        FILE *fpipe = NULL;
        char *command = "xcode-select -print-path";

        if (!(fpipe = (FILE *)popen(command, "r")))
            on_sys_error("Error encountered while opening pipe");

        char buffer[256] = { '\0' };

        fgets(buffer, sizeof(buffer), fpipe);
        pclose(fpipe);

        strtok(buffer, "\n");
        strcpy(xcode_dev_path, buffer);
    }
    return CFStringCreateWithCString(NULL, xcode_dev_path, kCFStringEncodingUTF8);
}

const char *get_home() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd *pwd = getpwuid(getuid());
        home = pwd->pw_dir;
    }
    return home;
}

CFStringRef copy_xcode_path_for(CFStringRef subPath, CFStringRef search) {
    CFStringRef xcodeDevPath = copy_xcode_dev_path();
    CFStringRef path;
    bool found = false;
    const char* home = get_home();
    CFRange slashLocation;


    // Try using xcode-select --print-path
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@/%@"), xcodeDevPath, subPath, search);
        found = path_exists(path);
    }
    // Try find `xcode-select --print-path` with search as a name pattern
    if (!found) {
        slashLocation = CFStringFind(search, CFSTR("/"), 0);
        if (slashLocation.location == kCFNotFound) {
        path = find_path(CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, subPath), search, CFSTR("-maxdepth 1"));
        } else {
             path = find_path(CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, subPath), search, CFSTR(""));
        }
        found = CFStringGetLength(path) > 0 && path_exists(path);
    }
    // If not look in the default xcode location (xcode-select is sometimes wrong)
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Applications/Xcode.app/Contents/Developer/%@&%@"), subPath, search);
        found = path_exists(path);
    }
    // If not look in the users home directory, Xcode can store device support stuff there
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/%@/%@"), home, subPath, search);
        found = path_exists(path);
    }

    CFRelease(xcodeDevPath);

    if (found) {
        return path;
    } else {
        CFRelease(path);
        return NULL;
    }
}

#define GET_FRIENDLY_MODEL_NAME(VALUE, INTERNAL_NAME, FRIENDLY_NAME)  if (kCFCompareEqualTo  == CFStringCompare(VALUE, CFSTR(INTERNAL_NAME), kCFCompareNonliteral)) { return CFSTR( FRIENDLY_NAME); };


// Please ensure that device is connected or the name will be unknown
const CFStringRef get_device_hardware_name(const AMDeviceRef device) {
    CFStringRef model = AMDeviceCopyValue(device, 0, CFSTR("HardwareModel"));

    if (model == NULL) {
        return CFSTR("Unknown Device");
    }

    // iPod Touch

    GET_FRIENDLY_MODEL_NAME(model, "N45AP",  "iPod Touch")
    GET_FRIENDLY_MODEL_NAME(model, "N72AP",  "iPod Touch 2G")
    GET_FRIENDLY_MODEL_NAME(model, "N18AP",  "iPod Touch 3G")
    GET_FRIENDLY_MODEL_NAME(model, "N81AP",  "iPod Touch 4G")
    GET_FRIENDLY_MODEL_NAME(model, "N78AP",  "iPod Touch 5G")
    GET_FRIENDLY_MODEL_NAME(model, "N78AAP", "iPod Touch 5G")

    // iPad

    GET_FRIENDLY_MODEL_NAME(model, "K48AP",  "iPad")
    GET_FRIENDLY_MODEL_NAME(model, "K93AP",  "iPad 2")
    GET_FRIENDLY_MODEL_NAME(model, "K94AP",  "iPad 2 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "K95AP",  "iPad 2 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "K93AAP", "iPad 2 (Wi-Fi, revision A)")
    GET_FRIENDLY_MODEL_NAME(model, "J1AP",   "iPad 3")
    GET_FRIENDLY_MODEL_NAME(model, "J2AP",   "iPad 3 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "J2AAP",  "iPad 3 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "P101AP", "iPad 4")
    GET_FRIENDLY_MODEL_NAME(model, "P102AP", "iPad 4 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "P103AP", "iPad 4 (CDMA)")

    // iPad Mini

    GET_FRIENDLY_MODEL_NAME(model, "P105AP", "iPad mini")
    GET_FRIENDLY_MODEL_NAME(model, "P106AP", "iPad mini (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "P107AP", "iPad mini (CDMA)")

    // Apple TV

    GET_FRIENDLY_MODEL_NAME(model, "K66AP",  "Apple TV 2G")
    GET_FRIENDLY_MODEL_NAME(model, "J33AP",  "Apple TV 3G")
    GET_FRIENDLY_MODEL_NAME(model, "J33IAP", "Apple TV 3.1G")

    // iPhone

    GET_FRIENDLY_MODEL_NAME(model, "M68AP", "iPhone")
    GET_FRIENDLY_MODEL_NAME(model, "N82AP", "iPhone 3G")
    GET_FRIENDLY_MODEL_NAME(model, "N88AP", "iPhone 3GS")
    GET_FRIENDLY_MODEL_NAME(model, "N90AP", "iPhone 4 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N92AP", "iPhone 4 (CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N90BAP", "iPhone 4 (GSM, revision A)")
    GET_FRIENDLY_MODEL_NAME(model, "N94AP", "iPhone 4S")
    GET_FRIENDLY_MODEL_NAME(model, "N41AP", "iPhone 5 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N42AP", "iPhone 5 (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N48AP", "iPhone 5c (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N49AP", "iPhone 5c (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N51AP", "iPhone 5s (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N53AP", "iPhone 5s (Global/CDMA)")
    GET_FRIENDLY_MODEL_NAME(model, "N61AP", "iPhone 6 (GSM)")
    GET_FRIENDLY_MODEL_NAME(model, "N56AP", "iPhone 6 Plus")

    return model;
}

char * MYCFStringCopyUTF8String(CFStringRef aString) {
  if (aString == NULL) {
    return NULL;
  }

  CFIndex length = CFStringGetLength(aString);
  CFIndex maxSize =
  CFStringGetMaximumSizeForEncoding(length,
                                    kCFStringEncodingUTF8);
  char *buffer = (char *)malloc(maxSize);
  if (CFStringGetCString(aString, buffer, maxSize,
                         kCFStringEncodingUTF8)) {
    return buffer;
  }
  return NULL;
}

CFStringRef get_device_full_name(const AMDeviceRef device) {
    CFStringRef full_name = NULL,
                device_udid = AMDeviceCopyDeviceIdentifier(device),
                device_name = NULL,
                model_name = NULL;

    AMDeviceConnect(device);

    device_name = AMDeviceCopyValue(device, 0, CFSTR("DeviceName")),
    model_name = get_device_hardware_name(device);

    if (verbose)
    {
      char *devName = MYCFStringCopyUTF8String(device_name);
      CFShow([NSString stringWithFormat:@"Device Name: %s\n", devName]);
      free(devName);

      char *mdlName = MYCFStringCopyUTF8String(model_name);
      CFShow([NSString stringWithFormat:@"Model Name: %s\n", mdlName]);
      free(mdlName);
    }

    if(device_name != NULL && model_name != NULL)
    {
        full_name = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ '%@' (%@)"), model_name, device_name, device_udid);
    }
    else
    {
        full_name = CFStringCreateWithFormat(NULL, NULL, CFSTR("(%@ss)"), device_udid);
    }

    AMDeviceDisconnect(device);

    if(device_udid != NULL)
        CFRelease(device_udid);
    if(device_name != NULL)
        CFRelease(device_name);
    if(model_name != NULL)
        CFRelease(model_name);

    return full_name;
}

CFStringRef get_device_interface_name(const AMDeviceRef device) {
    // AMDeviceGetInterfaceType(device) 0=Unknown, 1 = Direct/USB, 2 = Indirect/WIFI
    switch(AMDeviceGetInterfaceType(device)) {
        case 1:
            return CFSTR("USB");
        case 2:
            return CFSTR("WIFI");
        default:
            return CFSTR("Unknown Connection");
    }
}

CFMutableArrayRef get_device_product_version_parts(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFArrayRef parts = CFStringCreateArrayBySeparatingStrings(NULL, version, CFSTR("."));
    CFMutableArrayRef result = CFArrayCreateMutableCopy(NULL, CFArrayGetCount(parts), parts);
    CFRelease(version);
    CFRelease(parts);
    return result;
}

CFStringRef copy_device_support_path(AMDeviceRef device) {
    CFStringRef version = NULL;
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;
    CFMutableArrayRef version_parts = get_device_product_version_parts(device);

    while (CFArrayGetCount(version_parts) > 0) {
        version = CFStringCreateByCombiningStrings(NULL, version_parts, CFSTR("."));
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("iOS DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (*)"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), version);
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest"), CFSTR(""));
        }
        CFRelease(version);
        if (path != NULL) {
            break;
        }
        CFArrayRemoveValueAtIndex(version_parts, CFArrayGetCount(version_parts) - 1);
    }

    CFRelease(version_parts);
    CFRelease(build);

    if (path == NULL)
        on_error("Unable to locate DeviceSupport directory. This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!");

    return path;
}

CFStringRef copy_developer_disk_image_path(AMDeviceRef device) {
    CFStringRef version = NULL;
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;
    CFMutableArrayRef version_parts = get_device_product_version_parts(device);

    while (CFArrayGetCount(version_parts) > 0) {
        version = CFStringCreateByCombiningStrings(NULL, version_parts, CFSTR("."));
    if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("iOS DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)/DeveloperDiskImage.dmg"), version, build));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ (%@)/DeveloperDiskImage.dmg"), version, build));
    }
        if (path == NULL) {
             path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("*/%@ (*)/DeveloperDiskImage.dmg"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport"), CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/DeveloperDiskImage.dmg"), version));
        }
        if (path == NULL) {
            path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest"), CFSTR("DeveloperDiskImage.dmg"));
        }
        CFRelease(version);
        if (path != NULL) {
            break;
        }
        CFArrayRemoveValueAtIndex(version_parts, CFArrayGetCount(version_parts) - 1);
    }

    CFRelease(version_parts);
    CFRelease(build);
    if (path == NULL)
        on_error("Unable to locate DeveloperDiskImage.dmg. This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!");

    return path;
}

void mount_callback(CFDictionaryRef dict, int arg) {
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));

    if (CFEqual(status, CFSTR("LookingUpImage"))) {
        printf("[  0%%] Looking up developer disk image\n");
    } else if (CFEqual(status, CFSTR("CopyingImage"))) {
        printf("[ 30%%] Copying DeveloperDiskImage.dmg to device\n");
    } else if (CFEqual(status, CFSTR("MountingImage"))) {
        printf("[ 90%%] Mounting developer disk image\n");
    }
}

void mount_developer_image(AMDeviceRef device) {
    CFStringRef ds_path = copy_device_support_path(device);
    CFStringRef image_path = copy_developer_disk_image_path(device);
    CFStringRef sig_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@.signature"), image_path);

    if (verbose) {
        printf("Device support path: %s\n", CFStringGetCStringPtr(ds_path, CFStringGetSystemEncoding()));
        printf("Developer disk image: %s\n", CFStringGetCStringPtr(image_path, CFStringGetSystemEncoding()));
    }
    CFRelease(ds_path);

    FILE* sig = fopen(CFStringGetCStringPtr(sig_path, kCFStringEncodingMacRoman), "rb");
    void *sig_buf = malloc(128);
    assert(fread(sig_buf, 1, 128, sig) == 128);
    fclose(sig);
    CFDataRef sig_data = CFDataCreateWithBytesNoCopy(NULL, sig_buf, 128, NULL);
    CFRelease(sig_path);

    CFTypeRef keys[] = { CFSTR("ImageSignature"), CFSTR("ImageType") };
    CFTypeRef values[] = { sig_data, CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(sig_data);

    int result = AMDeviceMountImage(device, image_path, options, &mount_callback, 0);
    if (result == 0) {
        printf("[ 95%%] Developer disk image mounted successfully\n");
    } else if (result == 0xe8000076 /* already mounted */) {
        printf("[ 95%%] Developer disk image already mounted\n");
    } else {
        on_error("Unable to mount developer disk image. (%x)", result);
    }

    CFRelease(image_path);
    CFRelease(options);
}

mach_error_t transfer_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    if (CFEqual(status, CFSTR("CopyingFile"))) {
        CFStringRef path = CFDictionaryGetValue(dict, CFSTR("Path"));

        if ((last_path == NULL || !CFEqual(path, last_path)) && !CFStringHasSuffix(path, CFSTR(".ipa"))) {
            printf("[%3d%%] Copying %s to device\n", percent / 2, CFStringGetCStringPtr(path, kCFStringEncodingMacRoman));
        }

        if (last_path != NULL) {
            CFRelease(last_path);
        }
        last_path = CFStringCreateCopy(NULL, path);
    }

    return 0;
}

mach_error_t install_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    printf("[%3d%%] %s\n", (percent / 2) + 50, CFStringGetCStringPtr(status, kCFStringEncodingMacRoman));
    return 0;
}

CFURLRef copy_device_app_url(AMDeviceRef device, CFStringRef identifier) {
    CFDictionaryRef result = nil;

    NSArray *a = [NSArray arrayWithObjects:
                  @"CFBundleIdentifier",			// absolute must
                  @"ApplicationDSID",
                  @"ApplicationType",
                  @"CFBundleExecutable",
                  @"CFBundleDisplayName",
                  @"CFBundleIconFile",
                  @"CFBundleName",
                  @"CFBundleShortVersionString",
                  @"CFBundleSupportedPlatforms",
                  @"CFBundleURLTypes",
                  @"CodeInfoIdentifier",
                  @"Container",
                  @"Entitlements",
                  @"HasSettingsBundle",
                  @"IsUpgradeable",
                  @"MinimumOSVersion",
                  @"Path",
                  @"SignerIdentity",
                  @"UIDeviceFamily",
                  @"UIFileSharingEnabled",
                  @"UIStatusBarHidden",
                  @"UISupportedInterfaceOrientations",
                  nil];

    NSDictionary *optionsDict = [NSDictionary dictionaryWithObject:a forKey:@"ReturnAttributes"];
	CFDictionaryRef options = (CFDictionaryRef)optionsDict;

    check_error(AMDeviceLookupApplications(device, options, &result));

    CFDictionaryRef app_dict = CFDictionaryGetValue(result, identifier);
    assert(app_dict != NULL);

    CFStringRef app_path = CFDictionaryGetValue(app_dict, CFSTR("Path"));
    assert(app_path != NULL);

    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, app_path, kCFURLPOSIXPathStyle, true);
    CFRelease(result);
    return url;
}

CFStringRef copy_disk_app_identifier(CFURLRef disk_app_url) {
    CFURLRef plist_url = CFURLCreateCopyAppendingPathComponent(NULL, disk_app_url, CFSTR("Info.plist"), false);
    CFReadStreamRef plist_stream = CFReadStreamCreateWithFile(NULL, plist_url);
    CFReadStreamOpen(plist_stream);
    CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, plist_stream, 0, kCFPropertyListImmutable, NULL, NULL);
    CFStringRef bundle_identifier = CFRetain(CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier")));
    CFReadStreamClose(plist_stream);

    CFRelease(plist_url);
    CFRelease(plist_stream);
    CFRelease(plist);

    return bundle_identifier;
}

void write_lldb_prep_cmds(AMDeviceRef device, CFURLRef disk_app_url) {
    CFStringRef ds_path = copy_device_support_path(device);
    CFStringRef symbols_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("'%@/Symbols'"), ds_path);

    CFMutableStringRef cmds = CFStringCreateMutableCopy(NULL, 0, LLDB_PREP_CMDS);
    CFRange range = { 0, CFStringGetLength(cmds) };

    CFStringFindAndReplace(cmds, CFSTR("{symbols_path}"), symbols_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFStringFindAndReplace(cmds, CFSTR("{ds_path}"), ds_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFMutableStringRef pmodule = CFStringCreateMutableCopy(NULL, 0, LLDB_FRUITSTRAP_MODULE);

    CFRange rangeLLDB = { 0, CFStringGetLength(pmodule) };
    CFStringRef exitcode_app_crash_str = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), exitcode_app_crash);
    CFStringFindAndReplace(pmodule, CFSTR("{exitcode_app_crash}"), exitcode_app_crash_str, rangeLLDB, 0);
    rangeLLDB.length = CFStringGetLength(pmodule);

    if (args) {
        CFStringRef cf_args = CFStringCreateWithCString(NULL, args, kCFStringEncodingASCII);
        CFStringFindAndReplace(cmds, CFSTR("{args}"), cf_args, range, 0);
        rangeLLDB.length = CFStringGetLength(pmodule);
        CFStringFindAndReplace(pmodule, CFSTR("{args}"), cf_args, rangeLLDB, 0);

        //printf("write_lldb_prep_cmds:args: [%s][%s]\n", CFStringGetCStringPtr (cmds,kCFStringEncodingMacRoman),
        //    CFStringGetCStringPtr(pmodule, kCFStringEncodingMacRoman));
        CFRelease(cf_args);
    } else {
        CFStringFindAndReplace(cmds, CFSTR("{args}"), CFSTR(""), range, 0);
        CFStringFindAndReplace(pmodule, CFSTR("{args}"), CFSTR(""), rangeLLDB, 0);
        //printf("write_lldb_prep_cmds: [%s][%s]\n", CFStringGetCStringPtr (cmds,kCFStringEncodingMacRoman),
        //    CFStringGetCStringPtr(pmodule, kCFStringEncodingMacRoman));
    }
    range.length = CFStringGetLength(cmds);

    CFStringRef bundle_identifier = copy_disk_app_identifier(disk_app_url);
    CFURLRef device_app_url = copy_device_app_url(device, bundle_identifier);
    CFStringRef device_app_path = CFURLCopyFileSystemPath(device_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{device_app}"), device_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFStringRef disk_app_path = CFURLCopyFileSystemPath(disk_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_app}"), disk_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFStringRef device_port = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), port);
    CFStringFindAndReplace(cmds, CFSTR("{device_port}"), device_port, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef device_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, device_app_url);
    CFStringRef device_container_path = CFURLCopyFileSystemPath(device_container_url, kCFURLPOSIXPathStyle);
    CFMutableStringRef dcp_noprivate = CFStringCreateMutableCopy(NULL, 0, device_container_path);
    range.length = CFStringGetLength(dcp_noprivate);
    CFStringFindAndReplace(dcp_noprivate, CFSTR("/private/var/"), CFSTR("/var/"), range, 0);
    range.length = CFStringGetLength(cmds);
    CFStringFindAndReplace(cmds, CFSTR("{device_container}"), dcp_noprivate, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef disk_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, disk_app_url);
    CFStringRef disk_container_path = CFURLCopyFileSystemPath(disk_container_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_container}"), disk_container_path, range, 0);

    char python_file_path[300] = "/tmp/fruitstrap_";
    char python_command[300] = "fruitstrap_";
    if(device_id != NULL) {
        strcat(python_file_path, device_id);
        strcat(python_command, device_id);
    }
    strcat(python_file_path, ".py");

    CFStringRef cf_python_command = CFStringCreateWithCString(NULL, python_command, kCFStringEncodingASCII);
    CFStringFindAndReplace(cmds, CFSTR("{python_command}"), cf_python_command, range, 0);
    range.length = CFStringGetLength(cmds);
    CFStringRef cf_python_file_path = CFStringCreateWithCString(NULL, python_file_path, kCFStringEncodingASCII);
    CFStringFindAndReplace(cmds, CFSTR("{python_file_path}"), cf_python_file_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFDataRef cmds_data = CFStringCreateExternalRepresentation(NULL, cmds, kCFStringEncodingASCII, 0);
    char prep_cmds_path[300] = PREP_CMDS_PATH;
    if(device_id != NULL)
        strcat(prep_cmds_path, device_id);
    FILE *out = fopen(prep_cmds_path, "w");
    fwrite(CFDataGetBytePtr(cmds_data), CFDataGetLength(cmds_data), 1, out);
    // Write additional commands based on mode we're running in
    const char* extra_cmds;
    if (!interactive)
    {
        if (justlaunch)
          extra_cmds = lldb_prep_noninteractive_justlaunch_cmds;
        else
          extra_cmds = lldb_prep_noninteractive_cmds;
    }
    else if (nostart)
        extra_cmds = lldb_prep_no_cmds;
    else
        extra_cmds = lldb_prep_interactive_cmds;
    fwrite(extra_cmds, strlen(extra_cmds), 1, out);
    fclose(out);

    CFDataRef pmodule_data = CFStringCreateExternalRepresentation(NULL, pmodule, kCFStringEncodingASCII, 0);

    out = fopen(python_file_path, "w");
    fwrite(CFDataGetBytePtr(pmodule_data), CFDataGetLength(pmodule_data), 1, out);
    fclose(out);

    CFRelease(cmds);
    if (ds_path != NULL) CFRelease(ds_path);
    CFRelease(bundle_identifier);
    CFRelease(device_app_url);
    CFRelease(device_app_path);
    CFRelease(disk_app_path);
    CFRelease(device_container_url);
    CFRelease(device_container_path);
    CFRelease(dcp_noprivate);
    CFRelease(disk_container_url);
    CFRelease(disk_container_path);
    CFRelease(cmds_data);
    CFRelease(cf_python_command);
    CFRelease(cf_python_file_path);
}

CFSocketRef server_socket;
CFSocketRef lldb_socket;
CFWriteStreamRef serverWriteStream = NULL;
CFWriteStreamRef lldbWriteStream = NULL;

int kill_ptree(pid_t root, int signum);
void
server_callback (CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info)
{
    int res;

    if (CFDataGetLength (data) == 0) {
        // close the socket on which we've got end-of-file, the server_socket.
        CFSocketInvalidate(s);
        CFRelease(s);
        return;
    }
    res = write (CFSocketGetNative (lldb_socket), CFDataGetBytePtr (data), CFDataGetLength (data));
}

void lldb_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info)
{
    //printf ("lldb: %s\n", CFDataGetBytePtr (data));

    if (CFDataGetLength (data) == 0) {
        // close the socket on which we've got end-of-file, the lldb_socket.
        CFSocketInvalidate(s);
        CFRelease(s);
        return;
    }
    write (gdbfd, CFDataGetBytePtr (data), CFDataGetLength (data));
}

void fdvendor_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info) {
    CFSocketNativeHandle socket = (CFSocketNativeHandle)(*((CFSocketNativeHandle *)data));

    assert (callbackType == kCFSocketAcceptCallBack);
    //PRINT ("callback!\n");

    lldb_socket  = CFSocketCreateWithNative(NULL, socket, kCFSocketDataCallBack, &lldb_callback, NULL);
    int flag = 1;
    int res = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(flag));
    assert(res == 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, lldb_socket, 0), kCFRunLoopCommonModes);

    CFSocketInvalidate(s);
    CFRelease(s);
}

void start_remote_debug_server(AMDeviceRef device) {

    check_error(AMDeviceStartService(device, CFSTR("com.apple.debugserver"), &gdbfd, NULL));
    assert(gdbfd > 0);

    /*
     * The debugserver connection is through a fd handle, while lldb requires a host/port to connect, so create an intermediate
     * socket to transfer data.
     */
    server_socket = CFSocketCreateWithNative (NULL, gdbfd, kCFSocketDataCallBack, &server_callback, NULL);
    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, server_socket, 0), kCFRunLoopCommonModes);

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_len = sizeof(addr4);
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    CFSocketRef fdvendor = CFSocketCreate(NULL, PF_INET, 0, 0, kCFSocketAcceptCallBack, &fdvendor_callback, NULL);

    if (port) {
        int yes = 1;
        setsockopt(CFSocketGetNative(fdvendor), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }

    CFDataRef address_data = CFDataCreate(NULL, (const UInt8 *)&addr4, sizeof(addr4));

    CFSocketSetAddress(fdvendor, address_data);
    CFRelease(address_data);
    socklen_t addrlen = sizeof(addr4);
    int res = getsockname(CFSocketGetNative(fdvendor),(struct sockaddr *)&addr4,&addrlen);
    assert(res == 0);
    port = ntohs(addr4.sin_port);

    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, fdvendor, 0), kCFRunLoopCommonModes);
}

void kill_ptree_inner(pid_t root, int signum, struct kinfo_proc *kp, int kp_len) {
    int i;
    for (i = 0; i < kp_len; i++) {
        if (kp[i].kp_eproc.e_ppid == root) {
            kill_ptree_inner(kp[i].kp_proc.p_pid, signum, kp, kp_len);
        }
    }
    if (root != getpid()) {
        kill(root, signum);
    }
}

int kill_ptree(pid_t root, int signum) {
    int mib[3];
    size_t len;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_ALL;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) == -1) {
        return -1;
    }

    struct kinfo_proc *kp = calloc(1, len);
    if (!kp) {
        return -1;
    }

    if (sysctl(mib, 3, kp, &len, NULL, 0) == -1) {
        free(kp);
        return -1;
    }

    kill_ptree_inner(root, signum, kp, len / sizeof(struct kinfo_proc));

    free(kp);
    return 0;
}

void killed(int signum) {
    // SIGKILL needed to kill lldb, probably a better way to do this.
    kill(0, SIGKILL);
    _exit(0);
}

void lldb_finished_handler(int signum)
{
    int status = 0;
    if (waitpid(child, &status, 0) == -1)
        perror("waitpid failed");
    _exit(WEXITSTATUS(status));
}

void bring_process_to_foreground() {
    if (setpgid(0, 0) == -1)
        perror("setpgid failed");

    signal(SIGTTOU, SIG_IGN);
    if (tcsetpgrp(STDIN_FILENO, getpid()) == -1)
        perror("tcsetpgrp failed");
    signal(SIGTTOU, SIG_DFL);
}

void setup_dummy_pipe_on_stdin(int pfd[2]) {
    if (pipe(pfd) == -1)
        perror("pipe failed");
    if (dup2(pfd[0], STDIN_FILENO) == -1)
        perror("dup2 failed");
}

void setup_lldb(AMDeviceRef device, CFURLRef url) {
    CFStringRef device_full_name = get_device_full_name(device),
    device_interface_name = get_device_interface_name(device);

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    check_error(AMDeviceValidatePairing(device));
    check_error(AMDeviceStartSession(device));

    printf("------ Debug phase ------\n");

    if(AMDeviceGetInterfaceType(device) == 2)
    {
        CFShow([NSString stringWithFormat:@"Cannot debug %@ over %@.\n", device_full_name, device_interface_name]);
        exit(0);
    }

    CFShow([NSString stringWithFormat:@"Starting debug of %@ connected through %@...\n", device_full_name, device_interface_name]);

    mount_developer_image(device);      // put debugserver on the device
    start_remote_debug_server(device);  // start debugserver
    write_lldb_prep_cmds(device, url);   // dump the necessary lldb commands into a file

    CFRelease(url);

    printf("[100%%] Connecting to remote debug server\n");
    printf("-------------------------\n");

    setpgid(getpid(), 0);
    signal(SIGHUP, killed);
    signal(SIGINT, killed);
    signal(SIGTERM, killed);
    // Need this before fork to avoid race conditions. For child process we remove this right after fork.
    signal(SIGLLDB, lldb_finished_handler);

    parent = getpid();
}

void launch_debugger(AMDeviceRef device, CFURLRef url) {
    setup_lldb(device, url);
    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();

        int pfd[2] = {-1, -1};
        if (isatty(STDIN_FILENO))
            // If we are running on a terminal, then we need to bring process to foreground for input
            // to work correctly on lldb's end.
            bring_process_to_foreground();
        else
            // If lldb is running in a non terminal environment, then it freaks out spamming "^D" and
            // "quit". It seems this is caused by read() on stdin returning EOF in lldb. To hack around
            // this we setup a dummy pipe on stdin, so read() would block expecting "user's" input.
            setup_dummy_pipe_on_stdin(pfd);

        char lldb_shell[400];
        sprintf(lldb_shell, LLDB_SHELL);
        if(device_id != NULL)
            strcat(lldb_shell, device_id);

        int status = system(lldb_shell); // launch lldb
        if (status == -1)
            perror("failed launching lldb");

        close(pfd[0]);
        close(pfd[1]);

        // Notify parent we're exiting
        kill(parent, SIGLLDB);
        // Pass lldb exit code
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        child = pid;
    } else {
        on_sys_error("Fork failed");
    }
}

void launch_debugger_and_exit(AMDeviceRef device, CFURLRef url) {
    setup_lldb(device,url);
    int pfd[2] = {-1, -1};
    if (pipe(pfd) == -1)
        perror("Pipe failed");
    int pid = fork();
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGLLDB, SIG_DFL);
        child = getpid();

        if (dup2(pfd[0],STDIN_FILENO) == -1)
            perror("dup2 failed");

        char lldb_shell[400];
        sprintf(lldb_shell, LLDB_SHELL);
        if(device_id != NULL)
            strcat(lldb_shell, device_id);

        int status = system(lldb_shell); // launch lldb
        if (status == -1)
            perror("failed launching lldb");

        close(pfd[0]);

        // Notify parent we're exiting
        kill(parent, SIGLLDB);
        // Pass lldb exit code
        _exit(WEXITSTATUS(status));
    } else if (pid > 0) {
        child = pid;
        if (verbose)
            printf("Waiting for child [Child: %d][Parent: %d]\n", child, parent);
    } else {
        on_sys_error("Fork failed");
    }
}

CFStringRef get_bundle_id(CFURLRef app_url)
{
    if (app_url == NULL)
        return NULL;

    CFURLRef url = CFURLCreateCopyAppendingPathComponent(NULL, app_url, CFSTR("Info.plist"), false);

    if (url == NULL)
        return NULL;

    CFReadStreamRef stream = CFReadStreamCreateWithFile(NULL, url);
    CFRelease(url);

    if (stream == NULL)
        return NULL;

    CFPropertyListRef plist = NULL;
    if (CFReadStreamOpen(stream) == TRUE) {
        plist = CFPropertyListCreateWithStream(NULL, stream, 0,
                                               kCFPropertyListImmutable, NULL, NULL);
    }
    CFReadStreamClose(stream);
    CFRelease(stream);

    if (plist == NULL)
        return NULL;

    const void *value = CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier"));
    CFStringRef bundle_id = NULL;
    if (value != NULL)
        bundle_id = CFRetain(value);

    CFRelease(plist);
    return bundle_id;
}


void read_dir(service_conn_t afcFd, afc_connection* afc_conn_p, const char* dir,
              void(*callback)(afc_connection *conn,const char *dir,int file))
{
    char *dir_ent;

    afc_connection afc_conn;
    if (!afc_conn_p) {
        afc_conn_p = &afc_conn;
        AFCConnectionOpen(afcFd, 0, &afc_conn_p);
    }

    printf("%s\n", dir);

    afc_dictionary* afc_dict_p;
    char *key, *val;
    int not_dir = 0;

    unsigned int code = AFCFileInfoOpen(afc_conn_p, dir, &afc_dict_p);
    if (code != 0) {
        // there was a problem reading or opening the file to get info on it, abort
        return;
    }

    while((AFCKeyValueRead(afc_dict_p,&key,&val) == 0) && key && val) {
        if (strcmp(key,"st_ifmt")==0) {
            not_dir = strcmp(val,"S_IFDIR");
            break;
        }
    }
    AFCKeyValueClose(afc_dict_p);

    if (not_dir) {
    	if (callback) (*callback)(afc_conn_p, dir, not_dir);
        return;
    }

    afc_directory* afc_dir_p;
    afc_error_t err = AFCDirectoryOpen(afc_conn_p, dir, &afc_dir_p);

    if (err != 0) {
        // Couldn't open dir - was probably a file
        return;
    } else {
        if (callback) (*callback)(afc_conn_p, dir, not_dir);
    }

    while(true) {
        err = AFCDirectoryRead(afc_conn_p, afc_dir_p, &dir_ent);

        if (err != 0 || !dir_ent)
            break;

        if (strcmp(dir_ent, ".") == 0 || strcmp(dir_ent, "..") == 0)
            continue;

        char* dir_joined = malloc(strlen(dir) + strlen(dir_ent) + 2);
        strcpy(dir_joined, dir);
        if (dir_joined[strlen(dir)-1] != '/')
            strcat(dir_joined, "/");
        strcat(dir_joined, dir_ent);
        read_dir(afcFd, afc_conn_p, dir_joined, callback);
        free(dir_joined);
    }

    AFCDirectoryClose(afc_conn_p, afc_dir_p);
}


// Used to send files to app-specific sandbox (Documents dir)
service_conn_t start_house_arrest_service(AMDeviceRef device) {
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    check_error(AMDeviceValidatePairing(device));
    check_error(AMDeviceStartSession(device));

    service_conn_t houseFd;

    if (bundle_id == NULL) {
        printf("Bundle id is not specified\n");
        exit(1);
    }

    CFStringRef cf_bundle_id = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);
    if (AMDeviceStartHouseArrestService(device, cf_bundle_id, 0, &houseFd, 0) != 0)
    {
        printf("Unable to find bundle with id: %s\n", bundle_id);
        exit(1);
    }

    check_error(AMDeviceStopSession(device));
    check_error(AMDeviceDisconnect(device));
    CFRelease(cf_bundle_id);

    return houseFd;
}

char* get_filename_from_path(char* path)
{
    char *ptr = path + strlen(path);
    while (ptr > path)
    {
        if (*ptr == '/')
            break;
        --ptr;
    }
    if (ptr+1 >= path+strlen(path))
        return NULL;
    if (ptr == path)
        return ptr;
    return ptr+1;
}

void* read_file_to_memory(char * path, size_t* file_size)
{
    struct stat buf;
    int err = stat(path, &buf);
    if (err < 0)
    {
        return NULL;
    }

    *file_size = buf.st_size;
    FILE* fd = fopen(path, "r");
    char* content = malloc(*file_size);
    if (fread(content, *file_size, 1, fd) != 1)
    {
        fclose(fd);
        return NULL;
    }
    fclose(fd);
    return content;
}

void list_files(AMDeviceRef device)
{
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_connection* afc_conn_p;
    if (AFCConnectionOpen(houseFd, 0, &afc_conn_p) == 0) {
        read_dir(houseFd, afc_conn_p, list_root?list_root:"/", NULL);
        AFCConnectionClose(afc_conn_p);
    }
}

int app_exists(AMDeviceRef device)
{
    if (bundle_id == NULL) {
        printf("Bundle id is not specified\n");
        return 1;
    }
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    check_error(AMDeviceValidatePairing(device));
    check_error(AMDeviceStartSession(device));

    CFStringRef cf_bundle_id = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);

    NSArray *a = [NSArray arrayWithObjects:@"CFBundleIdentifier", nil];
    NSDictionary *optionsDict = [NSDictionary dictionaryWithObject:a forKey:@"ReturnAttributes"];
    CFDictionaryRef options = (CFDictionaryRef)optionsDict;
    CFDictionaryRef result = nil;
    check_error(AMDeviceLookupApplications(device, options, &result));

    bool appExists = CFDictionaryContainsKey(result, cf_bundle_id);
    printf("%s", appExists ? "true\n" : "false\n");
    CFRelease(cf_bundle_id);

    check_error(AMDeviceStopSession(device));
    check_error(AMDeviceDisconnect(device));
    if (appExists)
    	return 0;
    return -1;
}

void list_bundle_id(AMDeviceRef device)
{
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    check_error(AMDeviceValidatePairing(device));
    check_error(AMDeviceStartSession(device));
    
    NSArray *a = [NSArray arrayWithObjects:@"CFBundleIdentifier", nil];
    NSDictionary *optionsDict = [NSDictionary dictionaryWithObject:a forKey:@"ReturnAttributes"];
    CFDictionaryRef options = (CFDictionaryRef)optionsDict;
    CFDictionaryRef result = nil;
    check_error(AMDeviceLookupApplications(device, options, &result));
    
    CFIndex count;
    count = CFDictionaryGetCount(result);
    const void *keys[count];
    CFDictionaryGetKeysAndValues(result, keys, NULL);
    for(int i = 0; i < count; ++i) {
        CFStringRef test = (CFStringRef)keys[i];
        const char * key =  CFStringGetCStringPtr((CFStringRef)keys[i], kCFStringEncodingASCII);
        printf("%s\n", key);
    }
    
    check_error(AMDeviceStopSession(device));
    check_error(AMDeviceDisconnect(device));
}

void copy_file_callback(afc_connection* afc_conn_p, const char *name,int file)
{
    const char *local_name=name;

    if (*local_name=='/') local_name++;

    if (*local_name=='\0') return;

    if (file) {
	afc_file_ref fref;
	int err = AFCFileRefOpen(afc_conn_p,name,1,&fref);

	if (err) {
	    fprintf(stderr,"AFCFileRefOpen(\"%s\") failed: %d\n",name,err);
	    return;
	}

	FILE *fp = fopen(local_name,"w");

	if (fp==NULL) {
	    fprintf(stderr,"fopen(\"%s\",\"w\") failer: %s\n",local_name,strerror(errno));
	    AFCFileRefClose(afc_conn_p,fref);
	    return;
	}

	char buf[4096];
	size_t sz=sizeof(buf);

	while (AFCFileRefRead(afc_conn_p,fref,buf,&sz)==0 && sz) {
	    fwrite(buf,sz,1,fp);
	    sz = sizeof(buf);
	}

	AFCFileRefClose(afc_conn_p,fref);
	fclose(fp);
    } else {
	if (mkdir(local_name,0777) && errno!=EEXIST)
	    fprintf(stderr,"mkdir(\"%s\") failed: %s\n",local_name,strerror(errno));
    }
}

void mkdirhier(char *path)
{
    char *slash;
    struct stat buf;

    if (path[0]=='.' && path[1]=='/') path+=2;

    if ((slash = strrchr(path,'/'))) {
	*slash = '\0';
	if (stat(path,&buf)==0) {
	    *slash = '/';
	    return;
	}
	mkdirhier(path);
	mkdir (path,0777);
	*slash = '/';
    }

    return;
}

void download_tree(AMDeviceRef device)
{
    service_conn_t houseFd = start_house_arrest_service(device);
    afc_connection* afc_conn_p = NULL;
    char *dirname = NULL;

    if (AFCConnectionOpen(houseFd, 0, &afc_conn_p) == 0)  do {

	if (target_filename) {
	    dirname = strdup(target_filename);
	    mkdirhier(dirname);
	    if (mkdir(dirname,0777) && errno!=EEXIST) {
		fprintf(stderr,"mkdir(\"%s\") failed: %s\n",dirname,strerror(errno));
		break;
	    }
	    if (chdir(dirname)) {
		fprintf(stderr,"chdir(\"%s\") failed: %s\n",dirname,strerror(errno));
		break;
	    }
	}

	read_dir(houseFd, afc_conn_p, list_root?list_root:"/", copy_file_callback);

    } while(0);

    if (dirname) free(dirname);
    if (afc_conn_p) AFCConnectionClose(afc_conn_p);
}

void upload_file(AMDeviceRef device) {
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_file_ref file_ref;

    afc_connection afc_conn;
    afc_connection* afc_conn_p = &afc_conn;
    AFCConnectionOpen(houseFd, 0, &afc_conn_p);

    //        read_dir(houseFd, NULL, "/", NULL);

    if (!target_filename)
    {
        target_filename = get_filename_from_path(upload_pathname);
    }

    size_t file_size;
    void* file_content = read_file_to_memory(upload_pathname, &file_size);

    if (!file_content)
    {
        printf("Could not open file: %s\n", upload_pathname);
        exit(-1);
    }

    // Make sure the directory was created
    {
        char *dirpath = strdup(target_filename);
        char *c = dirpath, *lastSlash = dirpath;
        while(*c) {
            if(*c == '/') {
                lastSlash = c;
            }
            c++;
        }
        *lastSlash = '\0';
        check_error(AFCDirectoryCreate(afc_conn_p, dirpath));
    }


    int ret = AFCFileRefOpen(afc_conn_p, target_filename, 3, &file_ref);
    if (ret == 0x000a) {
        printf("Cannot write to %s. Permission error.\n", target_filename);
        exit(1);
    }
    if (ret == 0x0009) {
        printf("Target %s is a directory.\n", target_filename);
        exit(1);
    }
    assert(ret == 0);
    assert(AFCFileRefWrite(afc_conn_p, file_ref, file_content, file_size) == 0);
    assert(AFCFileRefClose(afc_conn_p, file_ref) == 0);
    assert(AFCConnectionClose(afc_conn_p) == 0);

    free(file_content);
}

void make_directory(AMDeviceRef device) {
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_file_ref file_ref;

    afc_connection afc_conn;
    afc_connection* afc_conn_p = &afc_conn;
    AFCConnectionOpen(houseFd, 0, &afc_conn_p);

    assert(AFCDirectoryCreate(afc_conn_p, target_filename) == 0);
    assert(AFCConnectionClose(afc_conn_p) == 0);
}

void remove_path(AMDeviceRef device) {
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_file_ref file_ref;

    afc_connection afc_conn;
    afc_connection* afc_conn_p = &afc_conn;
    AFCConnectionOpen(houseFd, 0, &afc_conn_p);


    assert(AFCRemovePath(afc_conn_p, target_filename) == 0);
    assert(AFCConnectionClose(afc_conn_p) == 0);
}

void uninstall_app(AMDeviceRef device) {
    CFRetain(device); // don't know if this is necessary?

    printf("------ Uninstall phase ------\n");

    //Do we already have the bundle_id passed in via the command line? if so, use it.
    CFStringRef cf_uninstall_bundle_id = NULL;
    if (bundle_id != NULL)
    {
        cf_uninstall_bundle_id = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);
    } else {
        printf ("Error: you need to pass in the bundle id, (i.e. --bundle_id com.my.app)");
        exit(1);
    }

    if (cf_uninstall_bundle_id == NULL) {
        printf("Error: Unable to get bundle id from user command or package %s\n Uninstall failed\n", app_path);
    } else {
        AMDeviceConnect(device);
        assert(AMDeviceIsPaired(device));
        check_error(AMDeviceValidatePairing(device));
        check_error(AMDeviceStartSession(device));

        int code = AMDeviceSecureUninstallApplication(0, device, cf_uninstall_bundle_id, 0, NULL, 0);
        if (code == 0) {
            printf("[ OK ] Uninstalled package with bundle id %s\n", CFStringGetCStringPtr(cf_uninstall_bundle_id, CFStringGetSystemEncoding()));
        } else {
            printf("[ ERROR ] Could not uninstall package with bundle id %s\n", CFStringGetCStringPtr(cf_uninstall_bundle_id, CFStringGetSystemEncoding()));
        }
        check_error(AMDeviceStopSession(device));
        check_error(AMDeviceDisconnect(device));
    }
}

void handle_device(AMDeviceRef device) {
    //if (found_device)
    //    return; // handle one device only

    CFStringRef found_device_id = AMDeviceCopyDeviceIdentifier(device),
                device_full_name = get_device_full_name(device),
                device_interface_name = get_device_interface_name(device);
                
    if (detect_only) {
        CFShow([NSString stringWithFormat:@"[....] Found %@ connected through %@.\n", device_full_name, device_interface_name]);
        found_device = true;
        return;
    }
    if (device_id != NULL) {
        CFStringRef deviceCFSTR = CFStringCreateWithCString(NULL, device_id, kCFStringEncodingASCII);
        if (CFStringCompare(deviceCFSTR, found_device_id, kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            found_device = true;
            CFRelease(deviceCFSTR);
        } else {
            CFShow([NSString stringWithFormat:@"Skipping %@.\n", device_full_name]);
            return;
        }
    } else {
        device_id = MYCFStringCopyUTF8String(found_device_id);
        found_device = true;
    }

    CFShow([NSString stringWithFormat:@"[....] Using %@ (%@).\n", device_full_name, found_device_id]);

    if (command_only) {
        if (strcmp("list", command) == 0) {
            list_files(device);
        } else if (strcmp("upload", command) == 0) {
            upload_file(device);
        } else if (strcmp("download", command) == 0) {
            download_tree(device);    
        } else if (strcmp("mkdir", command) == 0) {
            make_directory(device);
        } else if (strcmp("rm", command) == 0) {
            remove_path(device);
        } else if (strcmp("exists", command) == 0) {
            exit(app_exists(device));
        } else if (strcmp("uninstall_only", command) == 0) {
            uninstall_app(device);
        } else if (strcmp("list_bundle_id", command) == 0) {
            list_bundle_id(device);
        }
        exit(0);
    }


    CFRetain(device); // don't know if this is necessary?

    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);
    CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFURLRef url = CFURLCopyAbsoluteURL(relative_url);

    CFRelease(relative_url);

    if (uninstall) {
        printf("------ Uninstall phase ------\n");

        //Do we already have the bundle_id passed in via the command line? if so, use it.
        CFStringRef cf_uninstall_bundle_id = NULL;
        if (bundle_id != NULL)
        {
            cf_uninstall_bundle_id = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);
        } else {
            cf_uninstall_bundle_id = get_bundle_id(url);
        }

        if (cf_uninstall_bundle_id == NULL) {
            printf("Error: Unable to get bundle id from user command or package %s\n Uninstall failed\n", app_path);
        } else {
            AMDeviceConnect(device);
            assert(AMDeviceIsPaired(device));
            check_error(AMDeviceValidatePairing(device));
            check_error(AMDeviceStartSession(device));

            int code = AMDeviceSecureUninstallApplication(0, device, cf_uninstall_bundle_id, 0, NULL, 0);
            if (code == 0) {
                printf("[ OK ] Uninstalled package with bundle id %s\n", CFStringGetCStringPtr(cf_uninstall_bundle_id, CFStringGetSystemEncoding()));
            } else {
                printf("[ ERROR ] Could not uninstall package with bundle id %s\n", CFStringGetCStringPtr(cf_uninstall_bundle_id, CFStringGetSystemEncoding()));
            }
            check_error(AMDeviceStopSession(device));
            check_error(AMDeviceDisconnect(device));
        }
    }

    if(install) {
        printf("------ Install phase ------\n");
        CFShow([NSString stringWithFormat:@"[  0%%] Found %@ connected through %@, beginning install\n", device_full_name, device_interface_name]);

        AMDeviceConnect(device);
        assert(AMDeviceIsPaired(device));
        check_error(AMDeviceValidatePairing(device));
        check_error(AMDeviceStartSession(device));


        // NOTE: the secure version doesn't seem to require us to start the AFC service
        service_conn_t afcFd;
        check_error(AMDeviceSecureStartService(device, CFSTR("com.apple.afc"), NULL, &afcFd));
        check_error(AMDeviceStopSession(device));
        check_error(AMDeviceDisconnect(device));

        CFStringRef keys[] = { CFSTR("PackageType") };
        CFStringRef values[] = { CFSTR("Developer") };
        CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        //assert(AMDeviceTransferApplication(afcFd, path, NULL, transfer_callback, NULL) == 0);
        check_error(AMDeviceSecureTransferPath(0, device, url, options, transfer_callback, 0));

        close(afcFd);



        AMDeviceConnect(device);
        assert(AMDeviceIsPaired(device));
        check_error(AMDeviceValidatePairing(device));
        check_error(AMDeviceStartSession(device));

        // // NOTE: the secure version doesn't seem to require us to start the installation_proxy service
        // // Although I can't find it right now, I in some code that the first param of AMDeviceSecureInstallApplication was a "dontStartInstallProxy"
        // // implying this is done for us by iOS already

        //service_conn_t installFd;
        //assert(AMDeviceSecureStartService(device, CFSTR("com.apple.mobile.installation_proxy"), NULL, &installFd) == 0);

        //mach_error_t result = AMDeviceInstallApplication(installFd, path, options, install_callback, NULL);
        check_error(AMDeviceSecureInstallApplication(0, device, url, options, install_callback, 0));

        // close(installFd);

        check_error(AMDeviceStopSession(device));
        check_error(AMDeviceDisconnect(device));

        CFRelease(path);
        CFRelease(options);

        printf("[100%%] Installed package %s\n", app_path);
    }

    if (!debug)
        exit(0); // no debug phase

    if (justlaunch)
        launch_debugger_and_exit(device, url);
    else
        launch_debugger(device, url);
}

void device_callback(struct am_device_notification_callback_info *info, void *arg) {
    switch (info->msg) {
        case ADNCI_MSG_CONNECTED:
            if(device_id != NULL || !debug || AMDeviceGetInterfaceType(info->dev) != 2) {
                handle_device(info->dev);
            } else if(best_device_match == NULL) {
                best_device_match = info->dev;
                CFRetain(best_device_match);
            }
        default:
            break;
    }
}

void timeout_callback(CFRunLoopTimerRef timer, void *info) {
    if ((!found_device) && (!detect_only))  {
        if(best_device_match != NULL) {
            handle_device(best_device_match);

            CFRelease(best_device_match);
            best_device_match = NULL;
        }

        if(!found_device)
            on_error("Timed out waiting for device.");
    }
    else
    {
      if (!debug) {
          printf("[....] No more devices found.\n");
      }

      if (detect_only && !found_device) {
          exit(exitcode_error);
          return;
      } else {
          int mypid = getpid();
          if ((parent != 0) && (parent == mypid) && (child != 0))
          {
              if (verbose)
              {
                  printf("Timeout. Killing child (%d) tree\n", child);
              }
              kill_ptree(child, SIGHUP);
          }
      }
      exit(0);
    }
}

void usage(const char* app) {
    printf(
        "Usage: %s [OPTION]...\n"
        "  -d, --debug                  launch the app in lldb after installation\n"
        "  -i, --id <device_id>         the id of the device to connect to\n"
        "  -c, --detect                 only detect if the device is connected\n"
        "  -b, --bundle <bundle.app>    the path to the app bundle to be installed\n"
        "  -a, --args <args>            command line arguments to pass to the app when launching it\n"
        "  -t, --timeout <timeout>      number of seconds to wait for a device to be connected\n"
        "  -u, --unbuffered             don't buffer stdout\n"
        "  -n, --nostart                do not start the app when debugging\n"
        "  -I, --noninteractive         start in non interactive mode (quit when app crashes or exits)\n"
        "  -L, --justlaunch             just launch the app and exit lldb\n"
        "  -v, --verbose                enable verbose output\n"
        "  -m, --noinstall              directly start debugging without app install (-d not required)\n"
        "  -p, --port <number>          port used for device, default: dynamic\n"
        "  -r, --uninstall              uninstall the app before install (do not use with -m; app cache and data are cleared) \n"
        "  -9, --uninstall_only         uninstall the app ONLY. Use only with -1 <bundle_id> \n"
        "  -1, --bundle_id <bundle id>  specify bundle id for list and upload\n"
        "  -l, --list                   list files\n"
        "  -o, --upload <file>          upload file\n"
        "  -w, --download               download app tree\n"
        "  -2, --to <target pathname>   use together with up/download file/tree. specify target\n"
        "  -D, --mkdir <dir>            make directory on device\n"
        "  -R, --rm <path>              remove file or directory on device (directories must be empty)\n"
        "  -V, --version                print the executable version \n"
        "  -e, --exists                 check if the app with given bundle_id is installed or not \n"
        "  -B, --list_bundle_id         list bundle_id \n",
        app);
}

void show_version() {
	printf("%s\n", APP_VERSION);
}

int main(int argc, char *argv[]) {
    static struct option longopts[] = {
        { "debug", no_argument, NULL, 'd' },
        { "id", required_argument, NULL, 'i' },
        { "bundle", required_argument, NULL, 'b' },
        { "args", required_argument, NULL, 'a' },
        { "verbose", no_argument, NULL, 'v' },
        { "timeout", required_argument, NULL, 't' },
        { "unbuffered", no_argument, NULL, 'u' },
        { "nostart", no_argument, NULL, 'n' },
        { "noninteractive", no_argument, NULL, 'I' },
        { "justlaunch", no_argument, NULL, 'L' },
        { "detect", no_argument, NULL, 'c' },
        { "version", no_argument, NULL, 'V' },
        { "noinstall", no_argument, NULL, 'm' },
        { "port", required_argument, NULL, 'p' },
        { "uninstall", no_argument, NULL, 'r' },
        { "uninstall_only", no_argument, NULL, '9'},
        { "list", optional_argument, NULL, 'l' },
        { "bundle_id", required_argument, NULL, '1'},
        { "upload", required_argument, NULL, 'o'},
        { "download", optional_argument, NULL, 'w'},
        { "to", required_argument, NULL, '2'},
        { "mkdir", required_argument, NULL, 'D'},
        { "rm", required_argument, NULL, 'R'},
        { "exists", no_argument, NULL, 'e'},
        { "list_bundle_id", no_argument, NULL, 'B'},
        { NULL, 0, NULL, 0 },
    };
    char ch;

    while ((ch = getopt_long(argc, argv, "VmcdvunrILeD:R:i:b:a:t:g:x:p:1:2:o:l::w::9::B::", longopts, NULL)) != -1)
    {
        switch (ch) {
        case 'm':
            install = 0;
            debug = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'i':
            device_id = optarg;
            break;
        case 'b':
            app_path = optarg;
            break;
        case 'a':
            args = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'u':
            unbuffered = 1;
            break;
        case 'n':
            nostart = 1;
            break;
        case 'I':
            interactive = false;
            break;
        case 'L':
            interactive = false;
            justlaunch = true;
            break;
        case 'c':
            detect_only = true;
            debug = 1;
            break;
        case 'V':
            show_version();
            return 0;
        case 'p':
            port = atoi(optarg);
            break;
        case 'r':
            uninstall = 1;
            break;
        case '9':
            command_only = true;
            command = "uninstall_only";
            break;
        case '1':
            bundle_id = optarg;
            break;
        case '2':
            target_filename = optarg;
            break;
        case 'o':
            command_only = true;
            upload_pathname = optarg;
            command = "upload";
            break;
        case 'l':
            command_only = true;
            command = "list";
            list_root = optarg;
            break;
        case 'w':
            command_only = true;
            command = "download";
            list_root = optarg;
            break;
        case 'D':
            command_only = true;
            target_filename = optarg;
            command = "mkdir";
            break;
        case 'R':
            command_only = true;
            target_filename = optarg;
            command = "rm";
            break;
        case 'e':
            command_only = true;
            command = "exists";
            break;
        case 'B':
            command_only = true;
            command = "list_bundle_id";
            break;
        default:
            usage(argv[0]);
            return exitcode_error;
        }
    }

    if (!app_path && !detect_only && !command_only) {
        usage(argv[0]);
        on_error("One of -[b|c|o|l|w|D|R|e|9] is required to proceed!");
    }

    if (unbuffered) {
        setbuf(stdout, NULL);
        setbuf(stderr, NULL);
    }

    if (detect_only && timeout == 0) {
        timeout = 5;
    }

    if (app_path) {
        if (access(app_path, F_OK) != 0) {
            on_sys_error("Can't access app path '%s'", app_path);
        }
    }

    AMDSetLogLevel(5); // otherwise syslog gets flooded with crap
    if (timeout > 0)
    {
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + timeout, 0, 0, 0, timeout_callback, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
        printf("[....] Waiting up to %d seconds for iOS device to be connected\n", timeout);
    }
    else
    {
        printf("[....] Waiting for iOS device to be connected\n");
    }

    struct am_device_notification *notify;
    AMDeviceNotificationSubscribe(&device_callback, 0, 0, NULL, &notify);
    CFRunLoopRun();
}
