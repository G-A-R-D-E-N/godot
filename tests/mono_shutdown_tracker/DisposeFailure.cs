using Godot;
using System.Runtime.CompilerServices;
using System.Threading;

public partial class FailingResource : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        if (disposing && DisposeFailure.TakeThrow()) throw new InvalidOperationException("planned failure before base disposal.");
        base.Dispose(disposing);
        DisposeFailure.Completed(this);
    }
}

public partial class OwnerApiResource : RefCounted
{
    protected override void Dispose(bool disposing)
    {
        if (disposing) DisposeFailure.OwnerBeforeBase(this);
        base.Dispose(disposing);
        if (disposing) DisposeFailure.OwnerAfterBase(this);
    }
}

internal static class DisposeFailure
{
    private static readonly ManualResetEventSlim Recovered = new();
    private static FailingResource? held;
    private static WeakReference<GodotObject>? tracked;
    private static IntPtr target;
    private static string label = "";
    private static int pendingThrow, nativeCalls, failed, passed;

    private static void Fail(string reason)
    {
        Interlocked.Exchange(ref failed, 1);
        GD.PrintErr($"FAIL {label}: " + reason);
    }

    private static void Pass(string what)
    {
        if (Volatile.Read(ref failed) == 0 && Interlocked.Exchange(ref passed, 1) == 0) GD.Print($"PASS {label}: " + what);
    }

    private static void Count(IntPtr pointer, bool finalizer, Action forward)
    {
        if (pointer != target) forward();
        else if (Interlocked.Increment(ref nativeCalls) == 1) forward();
        else Fail("second native dispose of the target.");
    }

    private static void Arm(string name)
    {
        label = name;
        DisposeShim.Handler = Count;
        DisposeShim.Install();
    }

    internal static bool TakeThrow() => Interlocked.Exchange(ref pendingThrow, 0) == 1;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void FailFirstDisposal()
    {
        held = new FailingResource();
        tracked = DisposeShim.Tracked(held);
        target = held.NativeInstance;
        pendingThrow = 1;
        try
        {
            held.Dispose();
            Fail("Dispose did not throw.");
        }
        catch (InvalidOperationException)
        {
        }
        if (held.NativeInstance == IntPtr.Zero) Fail("native pointer was released although disposal failed.");
        if (!DisposeShim.Instances().ContainsKey(tracked)) Fail("tracker entry was removed although disposal failed.");
        if (Volatile.Read(ref nativeCalls) != 0) Fail("native dispose ran although disposal failed.");
        try
        {
            if (held.GetReferenceCount() < 1) Fail("unexpected reference count after failed disposal.");
        }
        catch (ObjectDisposedException)
        {
            Fail("object became unusable after failed disposal.");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Drop() => held = null;

    internal static void StartThrow(bool byFinalizer)
    {
        Arm(byFinalizer ? "throw-finalizer" : "throw-shutdown");
        FailFirstDisposal();
        GD.Print($"READY {label} disposal failed before base disposal; native pointer and tracker entry still live.");
        if (byFinalizer)
        {
            Drop();
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
            GC.WaitForPendingFinalizers();
            if (!Recovered.Wait(TimeSpan.FromSeconds(5))) Fail("finalizer did not recover cleanup.");
        }
    }

    internal static void Completed(FailingResource resource)
    {
        if (resource.NativeInstance != IntPtr.Zero) Fail("native pointer remained after disposal.");
        if (Volatile.Read(ref nativeCalls) != 1) Fail($"native dispose count was {nativeCalls}.");
        if (DisposeShim.Instances().ContainsKey(tracked!)) Fail("tracker entry remained after disposal.");
        Pass(
            "failed pre-base disposal left the object live; the later "
            + (label == "throw-finalizer" ? "finalizer" : "shutdown")
            + " owned cleanup with one native dispose and one tracker removal."
        );
        Recovered.Set();
    }

    private static OwnerApiResource? owner;

    internal static void StartOwnerApi()
    {
        Arm("owner-api");
        owner = new OwnerApiResource();
        tracked = DisposeShim.Tracked(owner);
        target = owner.NativeInstance;
        owner.Dispose();
    }

    internal static void OwnerBeforeBase(OwnerApiResource resource)
    {
        try
        {
            if (resource.GetReferenceCount() < 1) Fail("unexpected reference count before base disposal.");
        }
        catch (ObjectDisposedException)
        {
            Fail("owner thread could not use the object before base disposal.");
        }
        var blocked = false;
        var other = new Thread(() =>
        {
            try
            {
                resource.GetReferenceCount();
            }
            catch (ObjectDisposedException)
            {
                blocked = true;
            }
        });
        other.Start();
        other.Join();
        if (!blocked) Fail("a competing thread used the object after disposal was owned.");
    }

    internal static void OwnerAfterBase(OwnerApiResource resource)
    {
        try
        {
            resource.GetReferenceCount();
            Fail("owner thread still used the object after native cleanup.");
        }
        catch (ObjectDisposedException)
        {
        }
        if (resource.NativeInstance != IntPtr.Zero) Fail("native pointer remained after disposal.");
        if (Volatile.Read(ref nativeCalls) != 1) Fail($"native dispose count was {nativeCalls}.");
        if (DisposeShim.Instances().ContainsKey(tracked!)) Fail("tracker entry remained after disposal.");
        Pass(
            "the owner used the object before base disposal, a competing thread was refused, and cleanup made one "
            + "native dispose and one tracker removal."
        );
    }
}
