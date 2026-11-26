using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using Godot.NativeInterop;

#nullable enable

namespace Godot
{
    internal static class DisposablesTracker
    {
        [UnmanagedCallersOnly]
        internal static void OnGodotShuttingDown()
        {
            try
            {
                OnGodotShuttingDownImpl();
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
            }
            finally
            {
                CloseFinalizerAdmission();
                WaitForInFlightFinalizerDisposals();
                lock (FinalizerAdmissionGate)
                    ShutdownRegistrations.Clear();
            }
        }

        private static void OnGodotShuttingDownImpl()
        {
            bool isStdoutVerbose;

            try
            {
                isStdoutVerbose = OS.IsStdOutVerbose();
            }
            catch (ObjectDisposedException)
            {
                // OS singleton already disposed. Maybe OnUnloading was called twice.
                isStdoutVerbose = false;
            }

            if (isStdoutVerbose)
                GD.Print("Unloading: Draining pending finalizers...");

            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: false);
            bool drained = WaitForPendingFinalizers(DrainWaitMilliseconds);

            List<GodotObject> godotObjects = BeginFinalizerShutdown(out var seen);
            bool settled = WaitForPendingFinalizers(SettleWaitMilliseconds);
            AppendTrackedGodotObjects(godotObjects, seen);

            if (!drained || !settled)
            {
                GD.PrintErr($"DisposablesTracker: finalizers still running after {(drained ? SettleWaitMilliseconds : DrainWaitMilliseconds)} ms; " +
                    "later finalizers skip their native frees.");
            }

            if (isStdoutVerbose)
                GD.Print("Unloading: Disposing tracked instances...");

            // Dispose Godot Objects first, and only then dispose other disposables
            // like StringName, NodePath, Godot.Collections.Array/Dictionary, etc.
            // The Godot Object Dispose() method may need any of the later instances.

            DisposeGodotObjects(godotObjects);
            DrainShutdownRegistrations();

            foreach (WeakReference<IDisposable> item in OtherInstances.Keys)
            {
                try
                {
                    if (item.TryGetTarget(out IDisposable? self))
                    {
                        ShutdownDisposalDepth++;
                        try
                        {
                            self.Dispose();
                        }
                        finally
                        {
                            ShutdownDisposalDepth--;
                        }
                    }
                }
                catch (Exception e)
                {
                    ExceptionUtils.LogException(e);
                }
            }

            DrainShutdownRegistrations();

            if (isStdoutVerbose)
                GD.Print("Unloading: Finished disposing tracked instances.");
        }

        private const int DrainWaitMilliseconds = 5000;
        private const int SettleWaitMilliseconds = 1000;

        internal static volatile bool ShuttingDown;
        private static readonly object FinalizerAdmissionGate = new();
        private static int InFlightFinalizerDisposals;
        private static List<GodotObject> ShutdownRegistrations = new();
        private static readonly Dictionary<WeakReference<GodotObject>, GodotObject> PendingFinalizerRegistrations = new();
        [ThreadStatic] private static int FinalizerAdmissionDepth;
        [ThreadStatic] private static List<WeakReference<GodotObject>>? PendingFinalizerKeys;
        [ThreadStatic] private static int ShutdownDisposalDepth;

        private static List<GodotObject> BeginFinalizerShutdown(out HashSet<WeakReference<GodotObject>> seen)
        {
            lock (FinalizerAdmissionGate)
            {
                var objects = new List<GodotObject>();
                seen = new HashSet<WeakReference<GodotObject>>();
                AppendTrackedGodotObjects(objects, seen);
                ShuttingDown = true;
                return objects;
            }
        }

        private static void AppendTrackedGodotObjects(List<GodotObject> objects, HashSet<WeakReference<GodotObject>> seen)
        {
            lock (FinalizerAdmissionGate)
            {
                foreach (WeakReference<GodotObject> item in GodotObjectInstances.Keys)
                {
                    if (!PendingFinalizerRegistrations.ContainsKey(item) && seen.Add(item) && item.TryGetTarget(out GodotObject? self))
                        objects.Add(self);
                }
            }
        }

        private static void DisposeGodotObjects(List<GodotObject> objects)
        {
            foreach (GodotObject self in objects)
            {
                try
                {
                    ShutdownDisposalDepth++;
                    try
                    {
                        self.DisposeForShutdown();
                    }
                    finally
                    {
                        ShutdownDisposalDepth--;
                    }
                }
                catch (Exception e)
                {
                    ExceptionUtils.LogException(e);
                }
            }
        }

        private static void DrainShutdownRegistrations()
        {
            while (true)
            {
                WaitForInFlightFinalizerDisposals();
                List<GodotObject> objects;
                lock (FinalizerAdmissionGate)
                {
                    if (ShutdownRegistrations.Count == 0)
                        return;
                    objects = ShutdownRegistrations;
                    ShutdownRegistrations = new List<GodotObject>();
                }
                DisposeGodotObjects(objects);
            }
        }

        internal static bool TryEnterFinalizerDisposal()
        {
            lock (FinalizerAdmissionGate)
            {
                if (ShuttingDown)
                    return false;
                InFlightFinalizerDisposals++;
                FinalizerAdmissionDepth++;
                return true;
            }
        }

        internal static void ExitFinalizerDisposal()
        {
            lock (FinalizerAdmissionGate)
            {
                FinalizerAdmissionDepth--;
                if (FinalizerAdmissionDepth == 0 && PendingFinalizerKeys != null)
                {
                    foreach (WeakReference<GodotObject> key in PendingFinalizerKeys)
                    {
                        if (PendingFinalizerRegistrations.Remove(key, out GodotObject? self) && ShuttingDown)
                            ShutdownRegistrations.Add(self);
                    }
                    PendingFinalizerKeys = null;
                }
                InFlightFinalizerDisposals--;
                Monitor.PulseAll(FinalizerAdmissionGate);
            }
        }

        private static void CloseFinalizerAdmission()
        {
            lock (FinalizerAdmissionGate)
            {
                ShuttingDown = true;
            }
        }

        private static void WaitForInFlightFinalizerDisposals()
        {
            lock (FinalizerAdmissionGate)
            {
                while (InFlightFinalizerDisposals > 0)
                    Monitor.Wait(FinalizerAdmissionGate);
            }
        }

        private sealed class FinalizerWait
        {
            public volatile bool Done;
        }

        private static bool WaitForPendingFinalizers(int timeoutMilliseconds)
        {
            var wait = new FinalizerWait();
            try
            {
                var thread = new Thread(() =>
                {
                    GC.WaitForPendingFinalizers();
                    wait.Done = true;
                })
                {
                    IsBackground = true,
                    Name = "Godot shutdown finalizer wait",
                };
                thread.Start();
            }
            catch (Exception e)
            {
                ExceptionUtils.LogException(e);
                return false;
            }
            var clock = Stopwatch.StartNew();
            while (!wait.Done)
            {
                if (clock.ElapsedMilliseconds >= timeoutMilliseconds)
                    return false;
                Thread.Sleep(1);
            }
            return true;
        }

        private static ConcurrentDictionary<WeakReference<GodotObject>, byte> GodotObjectInstances { get; } =
            new();

        private static ConcurrentDictionary<WeakReference<IDisposable>, byte> OtherInstances { get; } =
            new();

        public static void RegisterGodotObject(GodotObject godotObject, out WeakReference<GodotObject>? weakReferenceToSelf)
        {
            lock (FinalizerAdmissionGate)
            {
                var reference = new WeakReference<GodotObject>(godotObject, trackResurrection: true);
                weakReferenceToSelf = reference;
                GodotObjectInstances.TryAdd(reference, 0);
                if (FinalizerAdmissionDepth > 0)
                {
                    PendingFinalizerRegistrations.Add(reference, godotObject);
                    (PendingFinalizerKeys ??= new List<WeakReference<GodotObject>>()).Add(reference);
                }
                else if (ShuttingDown && ShutdownDisposalDepth > 0)
                    ShutdownRegistrations.Add(godotObject);
            }
        }

        public static WeakReference<IDisposable> RegisterDisposable(IDisposable disposable)
        {
            var weakReferenceToSelf = new WeakReference<IDisposable>(disposable);
            OtherInstances.TryAdd(weakReferenceToSelf, 0);
            return weakReferenceToSelf;
        }

        public static void UnregisterGodotObject(GodotObject godotObject, WeakReference<GodotObject> weakReferenceToSelf)
        {
            if (!GodotObjectInstances.TryRemove(weakReferenceToSelf, out _))
                throw new ArgumentException("Godot Object not registered.", nameof(weakReferenceToSelf));
        }

        public static void UnregisterDisposable(WeakReference<IDisposable> weakReference)
        {
            if (!OtherInstances.TryRemove(weakReference, out _))
                throw new ArgumentException("Disposable not registered.", nameof(weakReference));
        }
    }
}
