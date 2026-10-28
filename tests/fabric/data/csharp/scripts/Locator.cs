using System.Collections.Generic;
using System.Linq;
using Godot;

// GetNode with the path forms Godot has: relative, absolute, unique name, typed and optional.
public partial class Locator : Node3D
{
    [Export] public Node3D Target;
    [Export] public NodePath SpawnPath = new NodePath("../Spawn");

    public Node3D Child;
    public Node Parent;
    public Node3D Spawn;
    public Node Missing = new Node();
    public Node Unique;
    public List<string> ChildNames = new();

    public override void _Ready()
    {
        Child = GetNode<Node3D>("Body/Mesh");
        Parent = GetParent();
        Spawn = GetNodeOrNull<Node3D>(SpawnPath);
        Missing = GetNodeOrNull<Node>("Nothing/Here");
        Unique = GetNodeOrNull<Node>("%Special");
        var root = GetNode("/root");
        GD.Print(root.Name);

        ChildNames = GetChildren().Select(c => c.Name.ToString()).ToList();
        if (HasNode("Body"))
        {
            Node body = FindChild("Mesh", true, false);
            GD.Print(body?.GetPath());
        }
        foreach (Node n in GetTree().GetNodesInGroup("targets"))
        {
            GD.Print(n.Name);
        }
        Target?.AddToGroup("seen");
    }
}
