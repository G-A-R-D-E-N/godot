using Godot;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

internal static class FinalizerRegistration
{
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Release = new();
    private static readonly ManualResetEventSlim SweepStarted = new();
    private static readonly List<FinalizerRegistrationSentinel> Sentinels = new();
    private static readonly List<RefCounted> Producers = new();
    private static IntPtr producer;
    private static WeakReference<GodotObject>? producerKey;
    private static IntPtr child;
    private static WeakReference<GodotObject>? childKey;
    private static int constructed;
    private static int nativeDisposals;
    private static int childDisposals;
    private static int failed;

    private static void Fail(string message)
    {
        Interlocked.Exchange(ref failed, 1);
        GD.PrintErr("FAIL finalizer-registration: " + message);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void MakeProducer()
    {
        for (int i = 0; i < 32; i++)
        {
            Producers.Add(new RefCounted());
            Sentinels.Add(new FinalizerRegistrationSentinel());
        }
        var keys = DisposeShim.Instances().Keys.ToList();
        int firstSentinel = Sentinels.Select(item => keys.IndexOf(DisposeShim.Tracked(item))).Where(index => index >= 0).Min();
        int selected = Producers.FindIndex(item => keys.IndexOf(DisposeShim.Tracked(item)) > firstSentinel);
        if (selected < 0)
            throw new Exception("Could not select a producer after a sentinel.");
        producer = Producers[selected].NativeInstance;
        producerKey = DisposeShim.Tracked(Producers[selected]);
        Producers.RemoveAt(selected);
    }

    private static void PlaceSentinel()
    {
        var instances = DisposeShim.Instances();
        for (int i = 0; i < 512; i++)
        {
            var sentinel = new FinalizerRegistrationSentinel();
            Sentinels.Add(sentinel);
            var keys = instances.Keys.ToList();
            int producerIndex = keys.IndexOf(producerKey!);
            if (producerIndex >= 0 && Sentinels.Any(item =>
                {
                    int index = keys.IndexOf(DisposeShim.Tracked(item));
                    return index >= 0 && index < producerIndex;
                }))
                return;
        }
        throw new Exception("Could not place a sentinel before the finalizer owner.");
    }

    internal static void Start()
    {
        DisposeShim.Handler = (pointer, finalizer, forward) =>
        {
            if (pointer == producer && finalizer)
                _ = new FinalizerRegistrationChild();
            else if (pointer == child && !finalizer)
                Interlocked.Increment(ref nativeDisposals);
            forward();
        };
        DisposeShim.Install();
        MakeProducer();
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        if (!Entered.Wait(TimeSpan.FromSeconds(5)))
            throw new Exception("Replacement constructor did not enter.");
        PlaceSentinel();
        GD.Print("READY finalizer-registration constructor held.");
        new Thread(ObserveShutdown) { IsBackground = true }.Start();
    }

    internal static void ChildConstructing(FinalizerRegistrationChild instance)
    {
        child = instance.NativeInstance;
        childKey = DisposeShim.Tracked(instance);
        Entered.Set();
        if (!Release.Wait(TimeSpan.FromSeconds(20)))
            Fail("constructor release timed out.");
        Volatile.Write(ref constructed, 1);
    }

    private static void ObserveShutdown()
    {
        try
        {
            Type tracker = typeof(GodotObject).Assembly.GetType("Godot.DisposablesTracker")
                ?? throw new Exception("Tracker unavailable.");
            FieldInfo flag = tracker.GetField("ShuttingDown", BindingFlags.NonPublic | BindingFlags.Static)
                ?? throw new Exception("Shutdown flag unavailable.");
            if (!SpinWait.SpinUntil(() => (bool)flag.GetValue(null)!, TimeSpan.FromSeconds(12)))
                throw new Exception("Shutdown admission did not close.");
            object gate = tracker.GetField("FinalizerAdmissionGate", BindingFlags.NonPublic | BindingFlags.Static)?.GetValue(null)
                ?? throw new Exception("Admission gate unavailable.");
            bool retained;
            lock (gate)
            {
                var pending = (IDictionary?)tracker.GetField("PendingFinalizerRegistrations", BindingFlags.NonPublic | BindingFlags.Static)?.GetValue(null)
                    ?? throw new Exception("Pending registration table unavailable.");
                retained = childKey != null && pending.Contains(childKey);
            }
            if (!retained)
                Fail("replacement was not retained while its constructor was active.");
            if (!SweepStarted.Wait(TimeSpan.FromSeconds(8)))
                throw new Exception("Shutdown did not begin the ordered object sweep.");
        }
        catch (Exception error)
        {
            Fail(error.Message);
        }
        finally
        {
            Release.Set();
        }
    }

    internal static void SentinelDisposed() => SweepStarted.Set();

    internal static void ChildDisposed(FinalizerRegistrationChild instance, bool disposing)
    {
        if (!disposing || Volatile.Read(ref constructed) == 0 || Interlocked.Increment(ref childDisposals) != 1)
            Fail("replacement was disposed before construction finished or more than once.");
        if (instance.NativeInstance != IntPtr.Zero || Volatile.Read(ref nativeDisposals) != 1 || childKey == null || DisposeShim.Instances().ContainsKey(childKey))
            Fail("replacement native ownership or tracker entry was not released exactly once.");
        if (Volatile.Read(ref failed) == 0)
            GD.Print("PASS finalizer-registration: construction finished before shutdown disposed the replacement.");
    }
}

public partial class FinalizerRegistrationSentinel : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing)
            FinalizerRegistration.SentinelDisposed();
    }
}

public partial class FinalizerRegistrationChild : RefCounted
{
    public FinalizerRegistrationChild() => FinalizerRegistration.ChildConstructing(this);

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        FinalizerRegistration.ChildDisposed(this, disposing);
    }
}
