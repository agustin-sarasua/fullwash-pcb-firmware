FullWash Firmware Installer
===========================

Follow these steps to install the firmware on your board:

1. Connect the board to your Mac with a USB-C cable.

2. Double-click "install.command".

3. If macOS says the file "cannot be opened because it is from an unidentified
   developer", right-click "install.command", choose Open, then click Open in
   the dialog. You only need to do this once.

4. The first time you run the installer, macOS may ask to install Xcode Command
   Line Tools. Click Install, wait for it to finish, then double-click
   "install.command" again.

5. Wait until you see "Done - you can unplug the board." That is it.

If nothing happens, the most likely cause is the USB-C cable. Some cables are
charge-only. Try a different cable that you know works for data transfer.

If the upload fails, a log file is saved at:
  ~/Library/Logs/FullWashInstaller.log
Send that file to support if you need help.
