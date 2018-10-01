# Download
## Chrome
Some high-level instructions are [here](https://chromium.googlesource.com/chromium/src/+/master/docs/android_build_instructions.md).
Here’s a shorter version:
```bash
export CHROMIUM_DIR=<location_for_chromium>
cd $CHROMIUM_DIR
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
# Modify your $PATH to include ~/depot_tools
mkdir ~/chromium && cd ~/chromium
fetch --nohooks android
```

## ANGLE
The following will checkout the specific version of ANGLE used with these
AOSP sources. The interface is expected to change over time.
```bash
cd $CHROMIUM_DIR/third_party/angle
git checkout 95277a300f52bf89b7a8c14ada10e4dd3c5962b5
```

## AOSP
Google docs on downloading, building and installing AOSP [here](https://source.android.com/setup/build/building).
Choose the “pie-angle-preview-dev” branch (which is based on “aosp-android-9.0.0_r8”),
based on available vendor binaries posted [here](https://developers.google.com/android/drivers#marlinppr2.180905.006.a1).
List of tagged branches [here](https://source.android.com/setup/start/build-numbers#source-code-tags-and-builds).

```bash
cd <location_for_aosp_build>
mkdir pie-angle-preview-dev
cd pie-angle-preview-dev
repo init --depth=1 -u https://android.googlesource.com/platform/manifest -b pie-angle-preview-dev && repo sync -q -j$(nproc)
```

## Pixel Binaries
The example downloads bits to build AOSP on a Google Pixel XL.
Download vendor and google binaries [here](https://developers.google.com/android/drivers#marlinppr2.180905.006.a1) into the top-level directory of your build (the pie-angle-preview-dev branch that you created above).
These must match the version of source being built.
Untar and extract binaries:
```bash
tar xvf qcom-marlin-ppr2.180905.006.a1-d11de9e0.tgz
tar xvf google_devices-marlin-ppr2.180905.006.a1-1090e880.tgz
./extract-qcom-marlin.sh
./extract-google_devices-marlin.sh
```

# Build
## ANGLE
```bash
cd $CHROMIUM_DIR/src
build/install-build-deps-android.sh
# Note: If the above step fails, you may need to manually install some packages yourself, using “sudo apt-get install”.  Then, re-run the above command to ensure it succeeds
gclient runhooks
gn gen out/Default64
gn args out/Default64
```
Manually edit the file: $CHROMIUM_DIR/out/ANGLEDebug/args.gn to set the following arguments:
```
target_os = "android"
target_cpu = "arm64"
is_debug = true
android32_ndk_api_level = 26
android64_ndk_api_level = 26
build_angle_deqp_tests = false
dcheck_always_on = true
ffmpeg_branding = "Chrome"
is_component_build = false
is_debug = true             # for debug build
proprietary_codecs = true
symbol_level = 1
angle_enable_vulkan = true
angle_enable_vulkan_validation_layers = false
angle_libs_suffix = "_angle"
build_apk_secondary_abi = true
dcheck_always_on = true
angle_enable_null = false
angle_force_thread_safety=true
```

```bash
ninja -v -C out/Debug64 third_party/angle:angle_apk
```

The above will create the ANGLE APK in the following location:
```bash
ls $CHROMIUM_DIR/out/ANGLEDebug/apks/ANGLEPrebuilt.apk
```

## AOSP
lunch targets documented [here](https://source.android.com/setup/build/running#flashing-a-device) (Note: Pixel XL is marlin, Pixel 2 is “walleye” and Pixel 2 XL is “taimen”).
```bash
source build/envsetup.sh
lunch aosp_marlin-userdebug
make -j dist
```
# Install
## ANGLE
We need to add the ANGLEPrebuilt.apk package to the system image. That can be done by creating the appropriate folder in $ANDROID_PRODUCT_OUT and putting the apk there and it will put into the appropriate system folder.
```bash
mkdir -p $ANDROID_PRODUCT_OUT/system/app/ANGLEPrebuilt
cp ANGLEPrebuilt.apk $ANDROID_PRODUCT_OUT/system/app/ANGLEPrebuilt
```
## AOSP
Now flash the image on to the device.
```bash
adb reboot bootloader
fastboot flashall -w
```
The following is needed to put the ANGLEPrebuilt package on the device. Can also use this to update the package on the device.
<wait for device to boot>
```bash
adb root
adb disable-verity
adb reboot
# wait for device to boot
adb root
adb remount
adb shell stop
adb sync
adb reboot
```


## Apps
Due to missing the Play Store on AOSP, for applications that can run on AOSP,
the user needs to download the apk from another device and install them on
their AOSP device.

For example, on a Pixel running Android Pie, connect the device to a computer,
install the desired application and use adb to download the apk.
Find the install location for the app. Should be in ```/data/apps/com.app.name-<random string>```
```bash
adb pull /data/apps/com.app.name-xxx ./app.apk
# Disconnect Android Pie device and connect AOSP device.
adb install app.apk
```
Use the same process for other applications that you want to test with ANGLE.

## Enable
In order to test out this Tech Preview of ANGLE, your AOSP build must disable preload of OpenGL ES drivers. To do that:
```bash
adb root
adb shell setprop ro.zygote.disable_gl_preload 1
adb shell stop && adb shell start
```

## Build-time selection
If you know at build time that you want your app to use ANGLE instead of native GLES libraries, there are two ways to do that.
Manifest method
Placing the following inside the <application> section of your AndroidManifest.xml will inform the GLES loader of your preference:

```
<meta-data
       android:name="com.android.angle.GLES_MODE"
       android:value="angle" />
```

## Rules method
The ANGLE apk contains a file with rules in it:


```bash
assets/a4a_rules.json
```

Modifying this file will influence the choice made by the GLES loader.

# Run-time selection
To enable ANGLE for an application you have two options.
 1. Open Settings -> System -> Developer Options -> Select ANGLE enabled app and pick the app you want to test with ANGLE. Only one application can be selected at a time.
 2. Set that setting via adb command:
    ``` adb shell settings put global angle_enabled_app <package-name> ```
    i.e.
    ```bash
    adb shell settings put global angle_enabled_app com.android.gl2jni
    ```

    Note that settings persist across reboot. To disable this setting:
    ```bash
    adb shell settings delete global angle_enabled_app
    ```

# Verify
To verify that the application is using ANGLE check logcat:
```bash
adb logcat
```
Check for the text: GraphicsEnvironment: <package-name> opted in for ANGLE via Developer Setting
That indicates that the named application was started and that the system saw that it was enabled to use ANGLE.
Applications that use GLSurfaceView use OpenGL in multiple threads. This has known issues and can cause an application to fail when ANGLE is enabled. A workaround that usually helps is to configure the GLSurfaceView to use Vulkan as it's render engine instead of OpenGL. This can be done with this command:
```bash
adb shell setprop debug.hwui.renderer skiavk
```
