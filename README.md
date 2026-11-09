# My contributions to this forked software are 100% cuck-coded, and thus will never reach a full 1.0 release because I have no idea how to code. You may treat this fork as a base from which a real coder can come in and refine this functionality.
So just, y'know...keep that in mind. I'm not gonna sit here and lie to you like I'm some big-shot software engineer.

Goes without saying, but this software isn't ready yet. It may never be ready. Go use NUSspli instead for now.

#LocaLeShop

Yeah so basically the idea is that you can host your own extracted WUXes on your server and have your Wii U console directly pull from there so you don't have to deal with file management yourself, it just works like an eShop.

# Features
- Download titles from local Samba servers.
- Install the downloaded titles to external or internal storage.
- Includes a keygen!
- Create fake tickets at will or if not found.
- Shows the download speed.
- On screen keyboard.
- Can download anything available on your NAS.
- Custom folder names for downloaded titles.
- Auto update.

# Install
To install the app, download and unzip the contents of the [latest release](https://github.com/TMStage/LocaLeShop/releases) and depending on how you will run the app, follow the next steps:

### Aroma
- Move NUSspli.wuhb to (SD:/wiiu/apps/).
- Run it from the HOME Menu.

### Channel
- Move the folder to (SD:/install/) and install it with WUPInstaller.
- Run it from the HOME Menu.

# Building
- Use `docker build -t nussplibuilder .` to build the container
- Use `docker run --rm -v ${PWD}:/project nussplibuilder python3 build.py` to build NUSspli

# Info
NUSspli is based on [WUPDownloader](https://github.com/Pokes303/WUPDownloader) by Pokes303.
