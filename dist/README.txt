Plugin Properties
=================

Adds "Plugin properties" to the Explorer right-click menu of Skyrim SE plugin
files (.esp, .esm, .esl). It opens a window styled like the Properties >
Details tab that shows the plugin's header version, record count, flags,
light status, author, description, masters, top-level GRUPs with their record
counts, and the file's size, dates and attributes.

Install
-------
1. Extract the whole zip (PluginPropertiesSetup.exe needs PluginProperties.dll
   next to it).
2. Run PluginPropertiesSetup.exe and answer Yes. It installs for the current
   user only, with no admin prompt, into
   %LOCALAPPDATA%\Programs\Plugin Properties

On Windows 11 the entry is in the classic menu: right-click > Show more
options (or Shift+right-click).

Uninstall
---------
Settings > Apps > Installed apps > Plugin Properties > Uninstall, or run
PluginPropertiesSetup.exe again and answer No.

Command line
------------
PluginPropertiesSetup.exe /install | /uninstall  [/allusers] [/quiet]
  /allusers  install for every user into Program Files (asks for admin)
  /quiet     no message boxes
