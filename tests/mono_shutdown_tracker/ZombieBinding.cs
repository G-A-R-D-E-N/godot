using Godot;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

internal static class ZombieBinding
{
    private static readonly string MeshPath = $"user://zombie_binding_mesh_{OS.GetProcessId()}.tres";
    private static readonly string ScenePath = $"user://zombie_binding_scene_{OS.GetProcessId()}.tscn";
    private static readonly string ScriptedPath = $"user://zombie_scripted_resource_{OS.GetProcessId()}.tres";
    private static readonly string ScriptedBinaryPath = $"user://zombie_scripted_binary_{OS.GetProcessId()}.res";
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Release = new();
    private static readonly TimeSpan GateWait = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan StressTime = TimeSpan.FromSeconds(15);
    private const int StressLoadsNeeded = 1000;

    private sealed class Gate
    {
        ~Gate()
        {
            Entered.Set();
            Release.Wait(GateWait);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void BlockFinalizers() => _ = new Gate();

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static IntPtr AssociatedHandle(IntPtr native)
    {
        var method = typeof(GodotObject).Assembly.GetType("Godot.NativeInterop.NativeFuncs")?.GetMethod(
            "godotsharp_internal_object_get_associated_gchandle", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static
        ) ?? throw new Exception("Associated handle query unavailable.");
        return (IntPtr)method.Invoke(null, new object[] { native })!;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Short, WeakReference Long, IntPtr Native, ulong Id) Prepare()
    {
        var mesh = new BoxMesh();
        Check(ResourceSaver.Save(mesh, MeshPath) == Error.Ok, "The mesh was not saved.");
        mesh.TakeOverPath(MeshPath);
        var node = new MeshInstance3D { Mesh = mesh };
        using (var packed = new PackedScene())
        {
            Check(packed.Pack(node) == Error.Ok, "The scene was not packed.");
            Check(ResourceSaver.Save(packed, ScenePath) == Error.Ok, "The scene was not saved.");
        }
        node.Free();
        return (new WeakReference(mesh, false), new WeakReference(mesh, true), mesh.NativeInstance, mesh.GetInstanceId());
    }

    private static (WeakReference Pending, IntPtr Native, ulong Id, PackedScene Scene) ReloadAfterCollection()
    {
        BlockFinalizers();
        GC.Collect();
        Check(Entered.Wait(GateWait), "The finalizer gate did not start.");
        var (weak, pending, native, id) = Prepare();
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        Check(!weak.IsAlive && pending.IsAlive, "Expected a cleared short reference and a pending mesh wrapper.");
        Check(AssociatedHandle(native) != IntPtr.Zero, "Expected the mesh binding to keep its weak handle before the reload.");
        var scene = ResourceLoader.Load<PackedScene>(ScenePath, null, ResourceLoader.CacheMode.Ignore);
        Check(scene != null, "The scene did not load.");
        Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the reload to find the mesh wrapper collected.");
        return (pending, native, id, scene!);
    }

    private static void Finish()
    {
        Release.Set();
        DirAccess.RemoveAbsolute(MeshPath);
        DirAccess.RemoveAbsolute(ScenePath);
    }

    private static void RunFinalizers()
    {
        Release.Set();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        GC.WaitForPendingFinalizers();
    }

    internal static void Run()
    {
        try
        {
            var (_, _, id, scene) = ReloadAfterCollection();
            RunFinalizers();
            GD.Print("READY zombie binding set up: the scene reload referenced a mesh whose wrapper was collected.");
            scene.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the scene to free the mesh.");
            GD.Print("PASS zombie: the scene released the last reference to that mesh.");
        }
        finally
        {
            Finish();
        }
    }

    internal static void RunRewrap()
    {
        try
        {
            var (pending, native, id, scene) = ReloadAfterCollection();
            var second = GodotObject.InstanceFromId(id) as RefCounted;
            Check(second != null && second.NativeInstance == native, "Expected a new wrapper for the mesh.");
            int references = second!.GetReferenceCount();
            var handle = AssociatedHandle(native);
            Check(handle != IntPtr.Zero && GCHandle.FromIntPtr(handle).Target == second, "Expected the binding to hold the new wrapper's handle.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old wrapper to be finalized.");
            Check(GodotObject.IsInstanceValid(second) && GodotObject.IsInstanceIdValid(id), "Expected the new wrapper and the mesh to stay valid after the old wrapper was finalized.");
            Check(second.GetReferenceCount() == references - 1, "Expected the old wrapper's finalizer to drop its one reference.");
            Check(AssociatedHandle(native) == handle && GCHandle.FromIntPtr(handle).Target == second, "Expected the binding to keep the new wrapper's handle.");
            GD.Print("READY zombie rewrap set up: the old wrapper was finalized while a new wrapper held the mesh.");
            second.Dispose();
            scene.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the scene to free the mesh.");
            GD.Print("PASS zombie-rewrap: the new wrapper stayed valid and the mesh was freed.");
        }
        finally
        {
            Finish();
        }
    }

    private static void SaveScripted(string path)
    {
        var parent = new ZombieScriptedParent { Value = 7 };
        parent.Items = new Godot.Collections.Array<ZombieScriptedChild> { new() { Chance = 5 }, new() { Chance = 6 } };
        Check(ResourceSaver.Save(parent, path) == Error.Ok, "The scripted resource was not saved.");
    }

    private static void CheckSaved(ZombieScriptedParent? parent, string when)
    {
        Check(parent != null, $"Expected {when} to return the resource.");
        Check(parent!.Value == 7 && parent.Items.Count == 2, $"Expected {when} to keep the saved values, got {parent.Value} and {parent.Items.Count} items.");
        for (var i = 0; i < parent.Items.Count; i++)
        {
            var child = parent.Items[i];
            Check(child != null, $"Expected {when} to return child {i}.");
            Check(child!.Chance == 5 + i, $"Expected {when} to keep child {i}'s saved value, got {child.Chance}.");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Short, WeakReference Long, IntPtr Native, ulong Id) LoadScripted(string path, ResourceLoader.CacheMode mode)
    {
        var parent = ResourceLoader.Load<ZombieScriptedParent>(path, null, mode);
        CheckSaved(parent, "the first load");
        return (new WeakReference(parent, false), new WeakReference(parent, true), parent.NativeInstance, parent.GetInstanceId());
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Short, WeakReference Long, IntPtr Native, ulong Id, string Path) LoadScriptedChild(List<ZombieScriptedChild> hold, string file)
    {
        var parent = ResourceLoader.Load<ZombieScriptedParent>(file);
        CheckSaved(parent, "the first load");
        hold.Add(parent.Items[0]);
        hold.Add(parent.Items[1]);
        return (new WeakReference(hold[0], false), new WeakReference(hold[0], true), hold[0].NativeInstance, hold[0].GetInstanceId(), hold[0].ResourcePath);
    }

    private static void FinishScripted()
    {
        Release.Set();
        DirAccess.RemoveAbsolute(ScriptedPath);
    }

    private static void HoldFinalizers()
    {
        BlockFinalizers();
        GC.Collect();
        Check(Entered.Wait(GateWait), "The finalizer gate did not start.");
    }

    private static void CollectToPending(WeakReference weak, WeakReference pending, IntPtr native, string path)
    {
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        Check(!weak.IsAlive && pending.IsAlive, "Expected a cleared short reference and a pending wrapper.");
        Check(ResourceLoader.HasCached(path), "Expected the cache to still list the path.");
        Check(AssociatedHandle(native) != IntPtr.Zero, "Expected the resource to keep its weak handle before the next reference.");
    }

    private static void AppendFromNative(Godot.Collections.Array holder, ulong id)
    {
        var expression = new Expression();
        Check(expression.Parse("h.append(instance_from_id(i))", new[] { "h", "i" }) == Error.Ok, expression.GetErrorText());
        expression.Execute(new Godot.Collections.Array { holder, (long)id });
        Check(!expression.HasExecuteFailed(), "Expected native code to reference the resource.");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void LoadForStress() => CheckSaved(ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath), "the load");

    internal static void RunScriptedStress()
    {
        try
        {
            SaveScripted(ScriptedPath);
            long loads = 0, failed = 0, pending = 0;
            string? first = null;
            var clock = Stopwatch.StartNew();
            while (clock.Elapsed < StressTime)
            {
                loads++;
                if (ResourceLoader.HasCached(ScriptedPath)) pending++;
                try
                {
                    LoadForStress();
                }
                catch (Exception error)
                {
                    failed++;
                    first ??= $"{error.GetType().Name}: {error.Message}";
                }
                GC.Collect();
            }
            Check(loads >= StressLoadsNeeded, $"Expected at least {StressLoadsNeeded} loads, got {loads}.");
            Check(pending > 0, $"Expected some of the {loads} loads to start with the collected wrapper's resource still in the cache.");
            GD.Print($"READY zombie scripted stress set up: {pending} of {loads} loads started with the collected wrapper's resource still in the cache.");
            Check(failed == 0, $"Expected every load to return the saved values: {failed} of {loads} did not, first {first}.");
            GD.Print("PASS zombie-scripted-stress: every load returned the saved values.");
        }
        finally
        {
            FinishScripted();
        }
    }

    internal static void RunScripted()
    {
        try
        {
            SaveScripted(ScriptedPath);
            HoldFinalizers();
            var (weak, pending, native, id) = LoadScripted(ScriptedPath, ResourceLoader.CacheMode.Reuse);
            CollectToPending(weak, pending, native, ScriptedPath);
            var fresh = ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath);
            CheckSaved(fresh, "the load after the collection");
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the load to find the wrapper collected.");
            var freshId = fresh.GetInstanceId();
            Check(freshId != id, "Expected the load to read the file again.");
            GD.Print("READY zombie scripted set up: a load found a scripted resource whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old wrapper to be finalized.");
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the finalizer to free the old resource.");
            Check(ReferenceEquals(ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath), fresh), "Expected the cache to hold the new resource.");
            fresh.Dispose();
            Check(!GodotObject.IsInstanceIdValid(freshId), "Expected the last reference to free the new resource.");
            GD.Print("PASS zombie-scripted: the load read the file again and the old resource was freed.");
        }
        finally
        {
            FinishScripted();
        }
    }

    internal static void RunScriptedChild()
    {
        try
        {
            SaveScripted(ScriptedPath);
            var hold = new List<ZombieScriptedChild>();
            var (weak, pending, native, id, path) = LoadScriptedChild(hold, ScriptedPath);
            GC.Collect();
            GC.WaitForPendingFinalizers();
            HoldFinalizers();
            hold.Clear();
            CollectToPending(weak, pending, native, path);
            var fresh = ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath);
            CheckSaved(fresh, "the load after the collection");
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the load to find the child wrapper collected.");
            Check(fresh.Items[0].GetInstanceId() != id, "Expected the load to read the child from the file again.");
            GD.Print("READY zombie scripted child set up: a load found a scripted child whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old child wrapper to be finalized.");
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the finalizer to free the old child.");
            CheckSaved(fresh, "the new resource after the finalizer");
            fresh.Dispose();
            GD.Print("PASS zombie-scripted-child: the load read the child from the file again and the old child was freed.");
        }
        finally
        {
            FinishScripted();
        }
    }

    internal static void RunScriptedHeld()
    {
        try
        {
            SaveScripted(ScriptedPath);
            HoldFinalizers();
            var (weak, pending, native, id) = LoadScripted(ScriptedPath, ResourceLoader.CacheMode.Reuse);
            CollectToPending(weak, pending, native, ScriptedPath);
            var holder = new Godot.Collections.Array();
            AppendFromNative(holder, id);
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the reference to find the wrapper collected.");
            Check(!ResourceLoader.HasCached(ScriptedPath), "Expected the reference to take the resource out of the cache.");
            holder.Clear();
            AppendFromNative(holder, id);
            Check(holder.Count == 1, "Expected native code to reference the resource again after dropping it.");
            var fresh = ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath);
            CheckSaved(fresh, "the load while native code holds the old resource");
            Check(fresh.GetInstanceId() != id, "Expected the load to read the file again.");
            GD.Print("READY zombie scripted held set up: native code referenced a scripted resource whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old wrapper to be finalized.");
            Check(GodotObject.IsInstanceIdValid(id), "Expected the held resource to stay alive with a new wrapper.");
            var held = GodotObject.InstanceFromId(id) as ZombieScriptedParent;
            Check(held != null && holder.Count == 1, "Expected the holder to keep the old resource.");
            Check(held!.Value == 0 && held.Items.Count == 0, "Expected the new wrapper to have default values.");
            Check(held.ResourcePath.Length == 0, "Expected the held resource to lose its path.");
            Check(ReferenceEquals(ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath), fresh), "Expected the cache to hold the resource read from the file.");
            holder.Clear();
            held.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the last reference to free the held resource.");
            fresh.Dispose();
            GD.Print("PASS zombie-scripted-held: the held resource got a new wrapper and the cache read the file again.");
        }
        finally
        {
            FinishScripted();
        }
    }

    internal static void RunScriptedIgnore()
    {
        try
        {
            SaveScripted(ScriptedPath);
            var kept = ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath);
            CheckSaved(kept, "the first load");
            HoldFinalizers();
            var (weak, pending, native, id) = LoadScripted(ScriptedPath, ResourceLoader.CacheMode.Ignore);
            Check(id != kept.GetInstanceId(), "Expected the ignoring load to make a copy.");
            CollectToPending(weak, pending, native, ScriptedPath);
            var holder = new Godot.Collections.Array();
            AppendFromNative(holder, id);
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the reference to find the copy's wrapper collected.");
            Check(ReferenceEquals(ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath), kept), "Expected the cache to keep the resource it already held.");
            GD.Print("READY zombie scripted ignore set up: native code referenced an uncached copy whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the copy's old wrapper to be finalized.");
            Check(ReferenceEquals(ResourceLoader.Load<ZombieScriptedParent>(ScriptedPath), kept), "Expected the cache to keep the resource after the finalizer.");
            CheckSaved(kept, "the kept resource after the finalizer");
            var held = GodotObject.InstanceFromId(id) as ZombieScriptedParent;
            Check(held != null && held.ResourcePath.Length == 0, "Expected the copy to lose its path.");
            holder.Clear();
            held!.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the last reference to free the copy.");
            kept.Dispose();
            GD.Print("PASS zombie-scripted-ignore: the uncached copy left the cached resource in place.");
        }
        finally
        {
            FinishScripted();
        }
    }

    internal static void RunScriptedReplace()
    {
        try
        {
            SaveScripted(ScriptedBinaryPath);
            HoldFinalizers();
            var (weak, pending, native, id) = LoadScripted(ScriptedBinaryPath, ResourceLoader.CacheMode.Reuse);
            CollectToPending(weak, pending, native, ScriptedBinaryPath);
            var fresh = ResourceLoader.Load<ZombieScriptedParent>(ScriptedBinaryPath, null, ResourceLoader.CacheMode.Replace);
            CheckSaved(fresh, "the replacing load after the collection");
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the replacing load to find the wrapper collected.");
            Check(fresh.GetInstanceId() != id, "Expected the replacing load to read the file into a new resource.");
            GD.Print("READY zombie scripted replace set up: a replacing load found a scripted resource whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old wrapper to be finalized.");
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the finalizer to free the old resource.");
            Check(ReferenceEquals(ResourceLoader.Load<ZombieScriptedParent>(ScriptedBinaryPath), fresh), "Expected the cache to hold the new resource.");
            fresh.Dispose();
            GD.Print("PASS zombie-scripted-replace: the replacing load read the binary file into a new resource and the old one was freed.");
        }
        finally
        {
            Release.Set();
            DirAccess.RemoveAbsolute(ScriptedBinaryPath);
        }
    }

    internal static void RunScriptedReplaceChild()
    {
        try
        {
            SaveScripted(ScriptedBinaryPath);
            var hold = new List<ZombieScriptedChild>();
            var (weak, pending, native, id, path) = LoadScriptedChild(hold, ScriptedBinaryPath);
            GC.Collect();
            GC.WaitForPendingFinalizers();
            HoldFinalizers();
            hold.Clear();
            CollectToPending(weak, pending, native, path);
            var fresh = ResourceLoader.Load<ZombieScriptedParent>(ScriptedBinaryPath, null, ResourceLoader.CacheMode.Replace);
            CheckSaved(fresh, "the replacing load after the collection");
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the replacing load to find the child wrapper collected.");
            Check(fresh.Items[0].GetInstanceId() != id, "Expected the replacing load to read the child from the file again.");
            GD.Print("READY zombie scripted replace child set up: a replacing load found a scripted child whose wrapper was collected.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old child wrapper to be finalized.");
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the finalizer to free the old child.");
            CheckSaved(fresh, "the new resource after the finalizer");
            fresh.Dispose();
            GD.Print("PASS zombie-scripted-replace-child: the replacing load read the child from the binary file again and the old child was freed.");
        }
        finally
        {
            Release.Set();
            DirAccess.RemoveAbsolute(ScriptedBinaryPath);
        }
    }
}
