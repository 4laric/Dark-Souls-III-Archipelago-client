# ⚠️ Warning ⚠️

This is no longer the latest version of the Dark Souls III Archipelago Randomizer! [This is now the latest repository](https://github.com/fswap/from-software-archipelago-clients).

# Dark-Souls-III-Archipelago-client

Dark Souls III client made for Archipelago multiworld randomizer. See [archipelago.gg] for
general information about Archipelago, and [the Dark Souls III setup guide] for instructions
on setting up and using this mod.

[archipelago.gg]: https://archipelago.gg/
[the Dark Souls III setup guide]: https://archipelago.gg/tutorial/Dark%20Souls%20III/setup_en
	
## Troubleshoots
- The provided dll requires other dependencies so if you encounter a crash when launching the game.
   - installing the latest Microsoft Visual C++ Redistributable version should fix it : https://aka.ms/vs/17/release/vc_redist.x64.exe.
- The Windows console tends to freeze preventing you from sending or receiving any items.
   - You must Alt+Tab, click on the console and press enter to refresh it.
- When trying to stream the Dark Souls 3 window to Discord when the Archipelago mod is used along ModEngine, the game crashes instantly with "Fatal Application Exit" window containing the ExpCode 0x80000003.
   - Change the modengine.ini file and set `blockNetworkAccess=0`

## Building Locally

To prepare your environment:

1. Install Visual Studio with C++ support.
2. Install [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started-vs):
   - Clone its repository (`git clone https://github.com/microsoft/vcpkg.git`).
   - Run its bootstrap script (`bootstrap-vcpkg.bat`).
   - Integrate it with Visual Studio (`vcpkg integrate install`).

To build the project:

1. Clone the repository (`git clone https://github.com/Marechal-L/Dark-Souls-III-Archipelago-client.git`).
2. Install submodules (`git submodule update --init --recursive`).
3. Open `archipelago-client\archipelago-client.sln` in Visual Studio.
4. Run Build > Build.

That's it! The mod will be in `x64\Debug\archipelago.dll`. Copy that over the file of the same
name from the released version of the mod and you can test against your local build.

## Credits
https://github.com/LukeYui/DS3-Item-Randomiser-OS by LukeYui  
https://github.com/black-sliver/apclientpp by black-sliver



