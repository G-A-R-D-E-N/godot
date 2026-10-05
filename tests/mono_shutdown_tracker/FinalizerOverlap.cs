using Godot;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

internal static unsafe class DisposeShim
{
    internal static Action<IntPtr, bool, Action>? Handler;
    private static IntPtr original;

    internal static void Install()
    {
        var table = typeof(GodotObject).Assembly.GetType("Godot.NativeInterop.NativeFuncs")?.GetField(
            "_unmanagedCallbacks", BindingFlags.NonPublic | BindingFlags.Static
        ) ?? throw new Exception("Native callback table unavailable.");
        var slot = table.FieldType.GetField(
            "godotsharp_internal_refcounted_disposed", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance
        ) ?? throw new Exception("Native refcounted dispose slot unavailable.");
        var callbacks = table.GetValue(null)!;
        original = (IntPtr)slot.GetValue(callbacks)!;
        slot.SetValue(callbacks, (IntPtr)(delegate* unmanaged<IntPtr, IntPtr, byte, void>)&Hook);
        table.SetValue(null, callbacks);
    }

    [UnmanagedCallersOnly]
    private static void Hook(IntPtr pointer, IntPtr handle, byte finalizer)
    {
        try
        {
            var next = original;
            Handler!(
                pointer,
                finalizer != 0,
                () => ((delegate* unmanaged<IntPtr, IntPtr, byte, void>)next)(pointer, handle, finalizer)
            );
        }
        catch (Exception error)
        {
            GD.PrintErr("FAIL dispose hook threw " + error);
        }
    }

    internal static WeakReference<GodotObject> Tracked(GodotObject resource) =>
        (WeakReference<GodotObject>?)typeof(GodotObject)
            .GetField("_weakReferenceToSelf", BindingFlags.NonPublic | BindingFlags.Instance)
            ?.GetValue(resource) ?? throw new Exception("GodotObject tracker reference unavailable.");

    internal static ConcurrentDictionary<WeakReference<GodotObject>, byte> Instances() =>
        (ConcurrentDictionary<WeakReference<GodotObject>, byte>)(
            typeof(GodotObject).Assembly.GetType("Godot.DisposablesTracker")
                ?.GetProperty("GodotObjectInstances", BindingFlags.NonPublic | BindingFlags.Static)
                ?.GetValue(null) ?? throw new Exception("Tracker registry unavailable.")
        );
}

public partial class OverlapResource : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        if (disposing) FinalizerOverlap.ShutdownReached();
        base.Dispose(disposing);
        if (disposing) FinalizerOverlap.ShutdownReturned();
    }
}

public partial class OverlapSentinel : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing) FinalizerOverlap.SentinelReturned();
    }
}

internal static class FinalizerOverlap
{
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Reached = new();
    private static readonly ManualResetEventSlim Release = new();
    private static readonly ManualResetEventSlim Exited = new();
    private static readonly ManualResetEventSlim Done = new();
    private static readonly ConcurrentDictionary<IntPtr, int> SentinelOrder = new();
    private static readonly HashSet<IntPtr> SentinelPointers = new();
    private static readonly List<OverlapSentinel> Sentinels = new();
    private static readonly List<WeakReference<GodotObject>> SentinelTracked = new();
    private static WeakReference<GodotObject>? targetTracked;
    private static IntPtr target;
    private static int sequence, targetCalls, reachedOrder, exitOrder, returnedOrder, sentinelReturns, failed, finished, later;

    private static int Next() => Interlocked.Increment(ref sequence);

    private static void Fail(string reason)
    {
        Interlocked.Exchange(ref failed, 1);
        GD.PrintErr("FAIL overlap: " + reason);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void MakeTarget()
    {
        var resource = new OverlapResource();
        targetTracked = DisposeShim.Tracked(resource);
        target = resource.NativeInstance;
    }

    private static void PlaceSentinels()
    {
        var instances = DisposeShim.Instances();
        for (var i = 0; i < 512; i++)
        {
            var sentinel = new OverlapSentinel();
            Sentinels.Add(sentinel);
            SentinelPointers.Add(sentinel.NativeInstance);
            SentinelTracked.Add(DisposeShim.Tracked(sentinel));
            if (i % 8 != 7) continue;
            var keys = instances.Keys.ToList();
            var targetIndex = keys.IndexOf(targetTracked!);
            later = SentinelTracked.Count(tracked => keys.IndexOf(tracked) > targetIndex);
            if (later >= 4 && SentinelTracked.Count - later >= 4) return;
        }
        throw new Exception("Sentinels could not be placed around the target.");
    }

    private static void Handle(IntPtr pointer, bool finalizer, Action forward)
    {
        if (pointer == target)
        {
            if (Interlocked.Increment(ref targetCalls) > 1)
            {
                Fail("second native dispose of the target.");
                return;
            }
            Entered.Set();
            if (!Release.Wait(TimeSpan.FromSeconds(15))) Fail("window gate timed out.");
            forward();
            Volatile.Write(ref exitOrder, Next());
            Exited.Set();
            TryFinish();
        }
        else if (!finalizer && SentinelPointers.Contains(pointer))
        {
            var order = Next();
            if (!SentinelOrder.TryAdd(pointer, order)) Fail("second native dispose of a sentinel.");
            var reached = Volatile.Read(ref reachedOrder);
            if (reached != 0 && order > reached && Volatile.Read(ref exitOrder) == 0)
            {
                Fail("shutdown disposed a later object while the finalizer was inside base disposal.");
                Release.Set();
            }
            forward();
            TryFinish();
        }
        else forward();
    }

    internal static void ShutdownReached()
    {
        Volatile.Write(ref reachedOrder, Next());
        Reached.Set();
    }

    internal static void ShutdownReturned()
    {
        Volatile.Write(ref returnedOrder, Next());
        TryFinish();
    }

    internal static void SentinelReturned()
    {
        Interlocked.Increment(ref sentinelReturns);
        TryFinish();
    }

    private static void TryFinish()
    {
        var exit = Volatile.Read(ref exitOrder);
        var returned = Volatile.Read(ref returnedOrder);
        if (exit == 0 || returned == 0 || Volatile.Read(ref sentinelReturns) != Sentinels.Count) return;
        if (Interlocked.Exchange(ref finished, 1) != 0) return;
        var instances = DisposeShim.Instances();
        if (Volatile.Read(ref targetCalls) != 1) Fail("target native dispose count was " + targetCalls + ".");
        if (returned < exit) Fail("shutdown returned before the finalizer completed base disposal.");
        if (instances.ContainsKey(targetTracked!)) Fail("target tracker entry remained.");
        if (SentinelTracked.Any(instances.ContainsKey)) Fail("sentinel tracker entries remained.");
        if (Volatile.Read(ref failed) == 0)
            GD.Print(
                $"PASS overlap: one native dispose of the target; shutdown waited for the finalizer; {later} of "
                + $"{Sentinels.Count} sentinels followed the target and all were disposed; tracker entries removed."
            );
        Done.Set();
    }

    internal static void Start()
    {
        DisposeShim.Handler = Handle;
        DisposeShim.Install();
        MakeTarget();
        PlaceSentinels();
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        if (!Entered.Wait(TimeSpan.FromSeconds(5))) throw new Exception("Finalizer did not enter base disposal.");
        GD.Print("READY overlap finalizer held inside base disposal.");
        new Thread(Watch) { IsBackground = true }.Start();
    }

    private static void Watch()
    {
        if (!Reached.Wait(TimeSpan.FromSeconds(10))) Fail("shutdown never reached the target.");
        Thread.Sleep(750);
        Release.Set();
        if (!Exited.Wait(TimeSpan.FromSeconds(5))) Fail("finalizer did not leave base disposal.");
        if (!Done.Wait(TimeSpan.FromSeconds(5))) Fail("shutdown did not finish disposing tracked objects.");
    }
}

internal static class DisposeRace
{
    private const int Iterations = 20000;
    private const int Threads = 3;

    internal static void Run()
    {
        var instances = DisposeShim.Instances();
        IntPtr current = IntPtr.Zero;
        int calls = 0, go = 0, done = 0, thrown = 0;
        GodotObject? resource = null;
        string? first = null;
        DisposeShim.Handler = (pointer, finalizer, forward) =>
        {
            if (pointer != Volatile.Read(ref current) || Interlocked.Increment(ref calls) == 1) forward();
        };
        DisposeShim.Install();
        var workers = Enumerable.Range(0, Threads).Select(_ => new Thread(() =>
        {
            for (var iteration = 1; iteration <= Iterations; iteration++)
            {
                while (Volatile.Read(ref go) < iteration) Thread.SpinWait(1);
                try
                {
                    Volatile.Read(ref resource)!.Dispose();
                }
                catch (Exception error)
                {
                    Interlocked.Increment(ref thrown);
                    Interlocked.CompareExchange(ref first, error.GetType().Name + ": " + error.Message, null);
                }
                Interlocked.Increment(ref done);
            }
        }) { IsBackground = true }).ToList();
        workers.ForEach(worker => worker.Start());
        for (var iteration = 1; iteration <= Iterations; iteration++)
        {
            var fresh = new BoxShape3D();
            var tracked = DisposeShim.Tracked(fresh);
            Volatile.Write(ref calls, 0);
            Volatile.Write(ref current, fresh.NativeInstance);
            Volatile.Write(ref resource, fresh);
            Volatile.Write(ref done, 0);
            Volatile.Write(ref go, iteration);
            while (Volatile.Read(ref done) < Threads) Thread.SpinWait(1);
            if (calls != 1 || thrown != 0 || instances.ContainsKey(tracked))
            {
                GD.PrintErr(
                    $"FAIL race: iteration {iteration} native disposes {calls}, exceptions {thrown} ({first}), "
                    + $"tracker entry present {instances.ContainsKey(tracked)}."
                );
                return;
            }
        }
        GD.Print(
            $"PASS race: {Iterations} iterations of {Threads} racing Dispose calls each made one native dispose "
            + "and one tracker removal without exceptions."
        );
    }
}
