using System;
using System.Collections.Concurrent;
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
        }

        private static void OnGodotShuttingDownImpl()
        {
            ShuttingDown = true;

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

            bool settled = WaitForPendingFinalizers(SettleWaitMilliseconds);

            if (!drained || !settled)
            {
                GD.PrintErr($"DisposablesTracker: finalizers still running after {(drained ? SettleWaitMilliseconds : DrainWaitMilliseconds)} ms; " +
                    "later finalizers skip their native frees.");
            }

            WaitForInFlightFinalizerDisposals();

            if (isStdoutVerbose)
                GD.Print("Unloading: Disposing tracked instances...");

            // Dispose Godot Objects first, and only then dispose other disposables
            // like StringName, NodePath, Godot.Collections.Array/Dictionary, etc.
            // The Godot Object Dispose() method may need any of the later instances.

            foreach (WeakReference<GodotObject> item in GodotObjectInstances.Keys)
            {
                try
                {
                    if (item.TryGetTarget(out GodotObject? self))
                        self.Dispose();
                }
                catch (Exception e)
                {
                    ExceptionUtils.LogException(e);
                }
            }

            foreach (WeakReference<IDisposable> item in OtherInstances.Keys)
            {
                try
                {
                    if (item.TryGetTarget(out IDisposable? self))
                        self.Dispose();
                }
                catch (Exception e)
                {
                    ExceptionUtils.LogException(e);
                }
            }

            if (isStdoutVerbose)
                GD.Print("Unloading: Finished disposing tracked instances.");
        }

        private const int DrainWaitMilliseconds = 5000;
        private const int SettleWaitMilliseconds = 1000;

        internal static volatile bool ShuttingDown;
        internal static int InFlightFinalizerDisposals;

        internal static void EnterFinalizerDisposal()
        {
            Interlocked.Increment(ref InFlightFinalizerDisposals);
        }

        internal static void ExitFinalizerDisposal()
        {
            Interlocked.Decrement(ref InFlightFinalizerDisposals);
        }

        private static void WaitForInFlightFinalizerDisposals()
        {
            var clock = Stopwatch.StartNew();
            while (Volatile.Read(ref InFlightFinalizerDisposals) > 0)
            {
                if (clock.ElapsedMilliseconds >= DrainWaitMilliseconds)
                {
                    return;
                }
                Thread.Sleep(1);
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

        public static WeakReference<GodotObject> RegisterGodotObject(GodotObject godotObject)
        {
            var weakReferenceToSelf = new WeakReference<GodotObject>(godotObject, trackResurrection: true);
            GodotObjectInstances.TryAdd(weakReferenceToSelf, 0);
            return weakReferenceToSelf;
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
