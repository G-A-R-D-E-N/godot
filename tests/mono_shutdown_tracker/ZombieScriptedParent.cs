using Godot;

public partial class ZombieScriptedParent : Resource
{
    [Export] public int Value { get; set; }
    [Export] public Godot.Collections.Array<ZombieScriptedChild> Items { get; set; } = new();
}
