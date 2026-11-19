Shmees VMX Probe 1.0
====================

What it does
------------
Shmees Media Center can use FFmpeg's vector (AltiVec/VMX) code to decode 1080p faster.
The Xbox 360's vector unit (VMX128) is missing some of the original VMX instructions, and
there is no public list of which ones. This small app tries every vector instruction that
FFmpeg's code uses, one at a time, and writes down which ones work.

It changes nothing on the console. It only writes two text files in its own folder:
  vmxprobe.txt          progress, written before and after each instruction
  vmxprobe-result.txt   the final list (the media center reads this one)

How to run it
-------------
1. Launch Hdd1:\Homebrew\ShmeesVMXProbe\ShmeesVMXProbe.xex from Aurora's file manager.
2. It shows each instruction's name as it tries it. That takes about 30 seconds.
3. When the screen says "Done", press Back to return to Aurora.

If the console freezes
----------------------
A freeze means that instruction does not exist on the 360. That is the expected way to
find one, and it does no harm.
1. Hold the power button until the console turns off.
2. Turn it back on, run your exploit as usual, and launch the probe again.
3. It sees where it stopped, marks that instruction as missing, and carries on from the
   next one.
Repeat until it says "Done".

After it's done
---------------
Open Shmees Media Center, go to Settings -> Diagnostics. The VIDEO section shows what the
probe found. If every instruction ran, press Y to run the vector test.

Title ID 0x53484D56. Built with xsdk (MIT). Source is in the Shmees Media Center source zip,
in the vmxprobe folder.
