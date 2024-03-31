// -----------------------------------------------------------------------------
// File: exit.ss
// Description: exit plugin (Debug Mode)
// Author: Alexandre Martins <http://opensurge2d.org>
// License: MIT
// -----------------------------------------------------------------------------

/*

This plugin lets the user exit the Debug Mode.

*/

object "Debug Mode - Exit" is "debug-mode-plugin"
{
    input = spawn("Debug Mode - Input");
    debugMode = null;

    state "main"
    {
        // return to the game if the back button is pressed
        if(input.buttonPressed("back"))
            debugMode.exit();
    }

    fun onLoad(debugModeObject)
    {
        debugMode = debugModeObject;
    }

    fun onUnload(debugModeObject)
    {
    }
}