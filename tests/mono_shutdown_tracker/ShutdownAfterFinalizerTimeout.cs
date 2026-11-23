using Godot;
using System.Runtime.CompilerServices;
using System.Threading;

internal static class ShutdownAfterFinalizerTimeout
{
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Release = new();
    private static IntPtr target;
    private static int nativeDisposes;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void MakeResource()
    {
        var resource = new TimeoutFinalizerResource();
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
        if (!Entered.Wait(TimeSpan.FromSeconds(5)))
            throw new Exception("Finalizer did not enter.");
        GD.Print("READY shutdown-after-finalizer-timeout held.");
        new Thread(() =>
        {
            // DrainWaitMilliseconds (5000) + SettleWaitMilliseconds (1000) + shutdown margin.
            Thread.Sleep(7500);
            Release.Set();
        }) { IsBackground = true }.Start();
    }

    internal static void BodyEntered()
    {
        Entered.Set();
        Release.Wait(TimeSpan.FromSeconds(30));
    }

    internal static void BodyExited()
    {
        if (Volatile.Read(ref nativeDisposes) == 0)
            GD.Print("PASS shutdown-after-finalizer-timeout: finalizer resumed after shutdown wait without native dispose.");
        else
            GD.PrintErr("FAIL shutdown-after-finalizer-timeout: native dispose ran after shutdown.");
    }
}

public partial class TimeoutFinalizerResource : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        if (!disposing)
        {
            ShutdownAfterFinalizerTimeout.BodyEntered();
            base.Dispose(disposing);
            ShutdownAfterFinalizerTimeout.BodyExited();
            return;
        }
        base.Dispose(disposing);
    }
}