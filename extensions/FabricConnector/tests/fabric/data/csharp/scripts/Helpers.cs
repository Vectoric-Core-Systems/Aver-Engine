using Godot;

public enum DoorState { Closed, Opening, Open }

public static class VectorExtensions
{
    public static Vector3 Flat(this Vector3 v) => new Vector3(v.X, 0, v.Z);

    public static T Find<T>(this Node node, string path) where T : Node => node.GetNodeOrNull<T>(path);
}

public struct Hit
{
    public Vector3 Point;
    public Vector3 Normal;
    public GodotObject Collider;
}

public class Pool<T> where T : Node, new()
{
    private readonly System.Collections.Generic.Stack<T> _free = new();

    public T Take() => _free.Count > 0 ? _free.Pop() : new T();

    public void Give(T item) => _free.Push(item);
}
