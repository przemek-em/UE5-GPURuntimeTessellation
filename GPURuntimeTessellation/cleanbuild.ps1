cd /d D:\UE\UnrealEngine-5.7.3-release
Remove-Item -Recurse -Force "Engine\Plugins\Experimental\Gpuruntimetessellation\Intermediate"
Remove-Item -Recurse -Force "Engine\Plugins\Experimental\Gpuruntimetessellation\Binaries"
.\Engine\Build\BatchFiles\Build.bat UnrealEditor Win64 Development -Plugin="D:\UE\UnrealEngine-5.7.3-release\Engine\Plugins\Experimental\GPURuntimeTessellation\GPURuntimeTessellation.uplugin" 