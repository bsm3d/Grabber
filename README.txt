Grabber: a small video capture tool for Windows (www.bsm3d.com, github.com/bsm3d/Grabber).

Records a ZONE you drag on the screen, a whole SCREEN, or a WINDOW, into an MP4 (H.264), with the sound of the PC when it plays
something and, if you tick it, your microphone (no sound track when nothing is heard).

  F5   start the recording
  F6   stop and save (the MP4 goes to the folder shown in the window, Videos\Grabber by default)
  F11  hide or show the tool (it is never in the recording)

The three keys work from any program, so the tool can be hidden while you record. If another program already uses one of them,
untick "F5, F6 and F11 work from any program": the keys then work only when the window is in front.

Options
- Compression (MP4, H.264 + AAC): Streaming (light file), Web (balanced), Full HD (best quality). All
  record at 30 images/s. The three have the same size, Full HD (1920 x 1080 at most: a bigger picture is shrunk to fit, never enlarged); only the
  compression changes.
- Applications: tick several programs (Unity, Houdini...) to record them in one video. The video shows the program that is in front
  and black while something else is in front (the desktop, another tool), so the desktop is never in the video.
- Webcam: the camera is shown small in the corner you choose (any of the four), 10 pixels from the edges.
- Microphone: your voice is recorded and mixed with the sound of the PC. With speakers the microphone can pick up the PC sound:
  use headphones.
- Encoding: automatic, nothing to choose. Grabber uses the graphics card when it can (NVIDIA, Intel, AMD or the Windows
  encoder) and the processor otherwise. If the graphics card fails, the processor takes over by itself.
- Show the clicks: a circle opens where the mouse is clicked (yellow for the left button, blue for the right one).
- Intro and outro: a text or an image, 1 to 15 seconds, before and after the recording. The text is drawn over the image.

Notes
- A WINDOW is recorded as its rectangle on the screen, brought to the front when the recording starts: it must stay where it is,
  and nothing should be put in front of it.
- The sound of the PC is what goes to the default speakers or headphones. The microphone is the default one, or the one you pick.
- Settings: %APPDATA%\BSM\Grabber\settings.ini (the saved intro and outro cards are in presets.json beside it).
