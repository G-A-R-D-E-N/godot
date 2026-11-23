using Godot;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

internal static class ShutdownResumeAfterCheck
{
    private static readonly ManualResetEventSlim Done = new();
    private static IntPtr target;
    private static int nativeDisposes;
    private static volatile bool shuttingDownSeen;

    private static bool ShuttingDown =>
        (bool)(typeof(GodotObject).Assembly.GetType("Godot.DisposablesTracker")
            ?.GetField("ShuttingDown", BindingFlags.NonPublic | BindingFlags.Static)
            ?.GetValue(null) ?? false);

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void MakeResource()
    {
        var resource = new ResumeAfterCheckResource();
        target = resource.NativeInstance;
    }

    internal static void Start()
    {
        DisposeShim.Handler = (pointer, finalizer, forward) =>
        {
            if (pointer == target && finalizer)
            {
                Interlocked.Increment(ref nativeDisposes);
            }
            forward();
        };
        DisposeShim.Install();
        MakeResource();
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        if (!ResumeAfterCheckResource.Entered.Wait(TimeSpan.FromSeconds(5)))
            throw new Exception("Finalizer did not enter.");
        GD.Print("READY shutdown-resume-after-check held.");
        new Thread(() =>
        {
            while (!ShuttingDown)
            {
                Thread.Sleep(1);
            }
            shuttingDownSeen = true;
            ResumeAfterCheckResource.Release.Set();
        }) { IsBackground = true }.Start();
    }

    internal static void Finish(bool aborted)
    {
        if (aborted)
        {
            Done.Set();
            return;
        }
        if (Volatile.Read(ref nativeDisposes) == 0 && shuttingDownSeen)
            GD.Print("PASS shutdown-resume-after-check: finalizer resumed after shutdown flag without native dispose.");
        else
            GD.PrintErr("FAIL shutdown-resume-after-check: native dispose or missed shutdown flag.");
        Done.Set();
    }

    internal static void AwaitDone()
    {
        if (!Done.Wait(TimeSpan.FromSeconds(20)))
            GD.PrintErr("FAIL shutdown-resume-after-check: timeout waiting for finalizer.");
    }
}

public partial class ResumeAfterCheckResource : RefCounted
{
    internal static readonly ManualResetEventSlim Entered = new();
    internal static readonly ManualResetEventSlim Release = new();

    protected override void Dispose(bool disposing)
    {
        if (!disposing)
        {
            Entered.Set();
            if (!Release.Wait(TimeSpan.FromSeconds(30)))
            {
                ShutdownResumeAfterCheck.Finish(true);
                return;
            }
            base.Dispose(disposing);
            ShutdownResumeAfterCheck.Finish(false);
            return;
        }
        base.Dispose(disposing);
    }
}