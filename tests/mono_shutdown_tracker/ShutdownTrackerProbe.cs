using Godot;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

public partial class ShutdownTrackerProbe : Node
{
    internal static readonly ManualResetEventSlim Entered = new();
    internal static readonly ManualResetEventSlim Release = new();
    internal static readonly ManualResetEventSlim Finished = new();
    internal static int PointerTransitions;

    private sealed class FinalizerGate
    {
        ~FinalizerGate()
        {
            Entered.Set();
            Release.Wait(TimeSpan.FromSeconds(15));
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void BlockFinalizers() => _ = new FinalizerGate();

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Short, WeakReference Long, WeakReference<GodotObject> Tracked) MakeResource(bool running)
    {
        GodotObject resource = running ? new RunningFinalizerResource() : new BoxShape3D();
        var field = typeof(GodotObject).GetField("_weakReferenceToSelf", BindingFlags.NonPublic | BindingFlags.Instance);
        var tracked = (WeakReference<GodotObject>?)field?.GetValue(resource)
            ?? throw new Exception("GodotObject tracker reference unavailable.");
        return (new WeakReference(resource, false), new WeakReference(resource, true), tracked);
    }

    public override void _Ready()
    {
        var tree = GetTree();
        try
        {
            var args = OS.GetCmdlineUserArgs();
            var running = args.Contains("--running-finalizer");
            GD.Print("ASSEMBLY " + typeof(GodotObject).Assembly.Location);
            if (args.Contains("--overlap-finalizer"))
            {
                FinalizerOverlap.Start();
                tree.Quit();
                return;
            }
            if (args.Contains("--race-dispose"))
            {
                DisposeRace.Run();
                tree.Quit();
                return;
            }
            if (!running)
            {
                BlockFinalizers();
                GC.Collect();
                if (!Entered.Wait(TimeSpan.FromSeconds(5))) throw new Exception("Finalizer gate did not start.");
            }
            var observation = MakeResource(running);
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
            if (running && !Entered.Wait(TimeSpan.FromSeconds(5))) throw new Exception("Resource finalizer did not enter.");
            if (observation.Short.IsAlive || !observation.Long.IsAlive)
                throw new Exception("Expected cleared short reference and pending long-reference target.");
            GD.Print("TRACKING " + observation.Tracked.TryGetTarget(out _));
            GD.Print(running ? "READY running finalizer held before base disposal." : "READY queued finalizer held.");
            if (args.Contains("--release-before-quit"))
            {
                Release.Set();
                GC.WaitForPendingFinalizers();
                if (running && PointerTransitions != 1) throw new Exception("Expected one native pointer transition.");
                GD.Print("CONTROL finalizer queue drained before shutdown.");
            }
            tree.Quit();
        }
        catch (Exception error)
        {
            Release.Set();
            GD.PrintErr("FAIL " + error);
            tree.Quit(1);
        }
    }
}

public partial class RunningFinalizerResource : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        if (!disposing)
        {
            ShutdownTrackerProbe.Entered.Set();
            if (!ShutdownTrackerProbe.Release.Wait(TimeSpan.FromSeconds(15)))
                GD.PrintErr("FAIL finalizer gate timed out.");
        }
        var before = NativeInstance;
        try
        {
            base.Dispose(disposing);
            if (before != IntPtr.Zero && NativeInstance == IntPtr.Zero)
                Interlocked.Increment(ref ShutdownTrackerProbe.PointerTransitions);
        }
        finally
        {
            if (!disposing) ShutdownTrackerProbe.Finished.Set();
        }
        if (disposing)
        {
            ShutdownTrackerProbe.Release.Set();
            if (!ShutdownTrackerProbe.Finished.Wait(TimeSpan.FromSeconds(5)))
                GD.PrintErr("FAIL finalizer did not complete during shutdown disposal.");
            if (ShutdownTrackerProbe.PointerTransitions != 1)
                GD.PrintErr("FAIL unexpected native pointer transition count.");
            else GD.Print("PASS shutdown native pointer transition once; finalizer completed.");
        }
    }
}
