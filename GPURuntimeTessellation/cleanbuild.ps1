cd /d Y:\UE\UnrealEngine-5.6.0-release
Remove-Item -Recurse -Force "Engine\Plugins\Experimental\Gpuruntimetessellation\Intermediate"
Remove-Item -Recurse -Force "Engine\Plugins\Experimental\Gpuruntimetessellation\Binaries"
.\Engine\Build\BatchFiles\Build.bat UnrealEditor Win64 Development -Plugin="Y:\UE\UnrealEngine-5.6.0-release\Engine\Plugins\Experimental\GPURuntimeTessellation\GPURuntimeTessellation.uplugin" 