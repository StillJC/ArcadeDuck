ArcadeDuck ReShade FX shaders
=============================

ArcadeDuck ships a deliberately small built-in ReShade FX set. The goal is a
known-good arcade-oriented baseline rather than a large unverified shader dump.

Built-in effects
----------------
crt/CRT-SatPixie-Simplified.fx
  Lightweight CRT simulation intended to work well with low-resolution 3D
  arcade content.

utility/Deband.fx
  Output debanding.

color/Curves.fx
color/Levels.fx
color/LiftGammaGain.fx
color/Tonemap.fx
color/Vibrance.fx
  General color and tone adjustment tools.

User effects
------------
ArcadeDuck also discovers user ReShade FX files recursively from:

  <DataRoot>\shaders\reshade\Shaders

External textures are loaded from:

  <DataRoot>\shaders\reshade\Textures

A user effect with the same shader name/path as a built-in effect takes
precedence over the bundled copy. Use Reload Shaders after changing files.

The built-in SGSR1, NIS and FSR1 display scalers are separate from the
post-processing shader chain.