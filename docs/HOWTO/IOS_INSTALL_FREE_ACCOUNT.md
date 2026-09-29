# Install on iPhone / iPad with a Free Apple Account

## Overview

This guide builds Zero Hour for iPhone or iPad on a Mac and installs it
with a **free** Apple ID (a "Personal Team"). No paid developer account is
needed.

What a free account means:

- **The app stops opening after 7 days.** Reinstall it with one command
  (see [Every 7 days](#every-7-days-reinstall)). No rebuild is needed.
- **Reinstalling keeps your saves and settings**, as long as you do not
  delete the app first. Deleting the app deletes its data.
- A free account can have at most 3 sideloaded apps on a device at once.

Paste commands **one block at a time** and check each one finishes before
running the next. Replace anything in `CAPITALS` with your own value, and
never type the `<` `>` brackets from examples: zsh treats them as file
redirection.

---

## One-time setup

### 1. Mac tools

1. Install **Xcode** from the Mac App Store, open it once, and sign in with
   your Apple ID under **Xcode → Settings → Accounts**.
2. Point the command-line tools at Xcode and add the iOS platform:

   ```sh
   sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
   sudo xcodebuild -license accept
   xcodebuild -runFirstLaunch
   xcodebuild -downloadPlatform iOS
   ```

   Check it worked. This must print a path ending in `iPhoneOS.sdk`:

   ```sh
   xcrun --sdk iphoneos --show-sdk-path
   ```

3. Install the build tools and vcpkg:

   ```sh
   brew install cmake ninja meson pkgconf xcodegen
   brew install --cask steamcmd
   git clone https://github.com/microsoft/vcpkg ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh
   ```

4. Install the **LunarG Vulkan SDK** from https://vulkan.lunarg.com/sdk/home
   (not the Homebrew one) and include the iOS component. Then see which
   version folder it created:

   ```sh
   ls ~/VulkanSDK
   ```

5. Save the environment settings. Use the version folder from step 4 in
   place of `VERSION`:

   ```sh
   echo 'setopt interactive_comments' >> ~/.zshrc
   echo 'export VCPKG_ROOT=$HOME/vcpkg' >> ~/.zshrc
   echo 'export VULKAN_SDK=$HOME/VulkanSDK/VERSION/macOS' >> ~/.zshrc
   source ~/.zshrc
   ```

   This must print a file path, not an error:

   ```sh
   ls $VULKAN_SDK/lib/MoltenVK.xcframework/ios-arm64/libMoltenVK.a
   ```

### 2. Get the code

```sh
mkdir -p ~/Games
cd ~/Games
git clone https://github.com/seastwood/Generals-Mac-iOS-iPad.git GeneralsX
cd GeneralsX
git submodule update --init --recursive references/fbraz3-dxvk
./scripts/build/ios/fetch-moltenvk.sh
./scripts/build/ios/stage-fonts.sh
```

`--recursive` matters: without it the build later fails with
`Missing Vulkan-Headers`.

### 3. Get the game files

You need your own copy of Zero Hour on Steam. This downloads it to
`~/GeneralsX/GeneralsZH` (about 2.7 GB):

```sh
cd ~/Games/GeneralsX
./scripts/get-assets.sh YOUR_STEAM_USERNAME
```

See [Getting the Game Files](GETTING_THE_GAME_FILES.md) for other options.

### 4. Build

```sh
cd ~/Games/GeneralsX
cmake --preset ios-vulkan
cmake --build build/ios-vulkan --target z_generals
```

The first `cmake --preset` run takes 30+ minutes while vcpkg builds FFmpeg
and other libraries. Lines with `warning:` are harmless; only `FAILED` or
`error:` matter. The build succeeded when it ends with
`Linking CXX executable GeneralsMD/GeneralsXZH.app/GeneralsXZH`.

### 5. Find your Team ID

Your Team ID is a 10-character code such as `476L3GD396`. It is **not** your
email address.

1. In **Xcode → Settings → Accounts**, select your Apple ID, click
   **Manage Certificates…**, then **+** → **Apple Development** (skip this if
   one is already listed).
2. Read the Team ID from that certificate:

   ```sh
   security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject
   ```

   The output looks like this:

   ```text
   subject=UID=..., CN=Apple Development: you@example.com (ZZZ6JY9YPW), OU=476L3GD396, O=Your Name, C=US
   ```

   Your Team ID is the value after **`OU=`** (`476L3GD396` here). The code in
   brackets after your email is a certificate ID, not the team, and signing
   fails with `No Account for Team` if you use it.

### 6. Prepare the device

1. Plug the iPhone or iPad into the Mac, unlock it, and tap **Trust**.
2. Turn on **Settings → Privacy & Security → Developer Mode** and let it
   restart. If the option is missing, open Xcode with the device plugged in
   (**Window → Devices and Simulators**) and it appears.
3. Find the device ID. It is the long `XXXXXXXX-XXXX...` value on your
   device's row:

   ```sh
   xcrun devicectl list devices
   ```

### 7. Save your signing settings

Save them once so later installs need no arguments. The file is git-ignored,
so it stays on your Mac. Pick any bundle ID that is unique to you:

```sh
cd ~/Games/GeneralsX
cat > ios/signing.env <<'EOF'
GX_TEAM_ID=YOUR_TEAM_ID
GX_BUNDLE_ID=com.YOURNAME.generalszh
GX_DEVICE_ID=YOUR_DEVICE_ID
EOF
```

`GX_DEVICE_ID` is optional: without it the script picks the first connected
device.

### 8. Package and install

With the device plugged in and unlocked:

```sh
cd ~/Games/GeneralsX
./scripts/build/ios/package-ios-zh.sh --install
```

A good run prints `bundled 2.7G of game data`, `signature OK`, and then
installs the app. It takes a few minutes.

If it fails with `No profiles for '...' were found`, let Xcode create the
profile once:

1. `open ios/GeneralsXZH.xcodeproj`
2. Click the **GeneralsXZH** target, then **Signing & Capabilities**.
3. Tick **Automatically manage signing**, choose your **(Personal Team)**,
   and set the Bundle Identifier to the same `GX_BUNDLE_ID` as in
   `ios/signing.env`.
4. Pick your device (not "Any iOS Device") at the top of the window and wait
   until the Signing section shows no errors.
5. Close Xcode and run the package command again.

Do **not** press ▶ Run in Xcode. That installs an empty placeholder app that
shows a black screen and closes.

### 9. Trust the app (first install only)

On the device, open **Settings → General → VPN & Device Management**, tap
your Apple ID, then tap **Trust**. Now open **Generals ZH** from the home
screen.

### Touch controls

- **Tap**: left click (select, command, press buttons). **Drag**: selection
  box. **Long-press**: right click. **Two-finger drag**: move the camera.
  **Pinch**: zoom. A short ring shows where each click landed (white: left,
  orange: right).
- **Edge scrolling**: rest a finger at the edge of the screen (not on the
  control bar or other buttons) to scroll the camera that way.
- **Hotkey toolbar** (in a game): tap the **Hotkeys** button in the top-right
  corner to open it and **Hide** (same place) to close it. It stays open until
  you close it, and is remembered next time. A button lights up white while
  pressed and flashes yellow when it fires (green for a long-press).
  - **All** / **Same**: select all combat units / all units of the selected
    type on screen. **Stop**, **Scatter**: order the selected units.
    **Home** / **Alert**: jump to your command center / the latest radar
    event. **Menu**: pause and options.
  - **1**-**5**: tap to select a group. To save the current selection as a
    group, tap **Ctrl** then the number (like Ctrl+1 on PC), or long-press
    the number. The button flashes green when a group is saved.
  - **Ctrl** / **Shift**: tap to hold the key for your next tap on the game
    (blue), long-press to lock it until you tap it again (orange). Ctrl+tap is
    force-attack, Shift+tap adds to a selection or queues waypoints.
  - **Opts**: button size, transparency (**Fade**), the keyboard button on or
    off, **2-Tap** (a quick second tap is a right click; off by default
    because the first tap is still a left click), edge scrolling, and the
    tap rings. Settings are saved in `Documents/touch-overlay.ini`.
- **Keyboard button** (the small translucent keyboard icon): tap it to show or
  hide the on-screen keyboard at any time. Long-press it, then drag, to move
  it; the position is remembered. While the keyboard is open the button moves
  up so it is never hidden behind the keyboard.
- The keyboard also opens by itself when a text field (such as a save name)
  is selected. Press **Return** or tap anywhere outside the text field to
  close it; tap the text field to bring it back. Autocorrect and the
  predictive text bar are turned off.

---

## Every 7 days: reinstall

When the app stops opening (iOS says it is no longer available), plug in and
unlock the device, then run:

```sh
cd ~/Games/GeneralsX
./scripts/build/ios/package-ios-zh.sh --install
```

That is all. It refreshes the 7-day signing profile and installs over the
existing app, so saves and settings are kept. Do not delete the app first.

If Xcode has signed you out, the command fails with `No Account for Team`:
sign in again under **Xcode → Settings → Accounts** and rerun it.

## After updating the code

To pick up new fixes, pull, rebuild, then reinstall:

```sh
cd ~/Games/GeneralsX
git pull
git submodule update --init --recursive references/fbraz3-dxvk
cmake --build build/ios-vulkan --target z_generals
./scripts/build/ios/package-ios-zh.sh --install
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `zsh: number expected` or `pathspec '#'` | You pasted a `# comment`. Run `setopt interactive_comments` or paste without comments. |
| `zsh: no such file or directory: your-team-id` | Replace the whole `<...>` placeholder, brackets included. |
| `Could not find toolchain file: "/scripts/buildsystems/vcpkg.cmake"` | `VCPKG_ROOT` is not set. Redo setup step 1.5. |
| `vcpkg was unable to detect the active compiler` / `SDK "iphoneos" cannot be located` | The tools point at Command Line Tools instead of Xcode. Redo setup step 1.2, then `rm -rf build/ios-vulkan` and configure again. |
| `Missing Vulkan-Headers` | Run `git submodule update --init --recursive references/fbraz3-dxvk`, then build again. |
| `No Account for Team "..."` | Wrong Team ID (use the `OU=` value), or Xcode signed you out. |
| `No profiles for '...' were found` | Let Xcode create the profile once (setup step 8). |
| `Apple Development: ambiguous` | Duplicate certificates in the keychain. The script now picks the right one automatically; you can also delete the duplicate in **Keychain Access → login → My Certificates**. |
| `The specified device was not found` | Set `GX_DEVICE_ID` in `ios/signing.env` (setup step 6.3). |
| App shows a black screen and closes at once | That is the empty placeholder from pressing Run in Xcode. Delete it and run the package command. |
| App crashes or misbehaves | Pull the game's log (below) and look at the last lines. |

To pull the game's log from the device after a session (use your own bundle
ID and device ID):

```sh
xcrun devicectl device copy from --device YOUR_DEVICE_ID \
  --domain-type appDataContainer --domain-identifier com.YOURNAME.generalszh \
  --source Documents/generals-stderr.log --destination ~/generals-stderr.log
tail -60 ~/generals-stderr.log
```

The previous session's log is `Documents/generals-stderr-prev.log`.
