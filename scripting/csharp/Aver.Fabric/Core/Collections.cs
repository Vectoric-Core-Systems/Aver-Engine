// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot.Collections: Array, Array<T>, Dictionary and Dictionary<K,V> as reference types over Variants.

using System.Collections;
using System.Text;

namespace Aver.Fabric.Collections;

/// <summary>A reference-type list of Variants (Godot's Array). Assigning one copies the reference, as in Godot.</summary>
public sealed class Array : IList<Variant>, IReadOnlyList<Variant>
{
    internal readonly List<Variant> List;

    public Array() => List = new List<Variant>();
    public Array(IEnumerable<Variant> collection) => List = new List<Variant>(collection);
    public Array(Variant[] array) => List = new List<Variant>(array);

    public int Count => List.Count;
    public bool IsReadOnly => false;

    public Variant this[int index]
    {
        get => List[index];
        set => List[index] = value;
    }

    public void Add(Variant item) => List.Add(item);
    public int AddRange(IEnumerable<Variant> items) { int n = List.Count; List.AddRange(items); return List.Count - n; }
    public void Clear() => List.Clear();
    public bool Contains(Variant item) => List.Contains(item);
    public void CopyTo(Variant[] array, int arrayIndex) => List.CopyTo(array, arrayIndex);
    public int IndexOf(Variant item) => List.IndexOf(item);
    public void Insert(int index, Variant item) => List.Insert(index, item);
    public bool Remove(Variant item) => List.Remove(item);
    public void RemoveAt(int index) => List.RemoveAt(index);
    public IEnumerator<Variant> GetEnumerator() => List.GetEnumerator();
    IEnumerator IEnumerable.GetEnumerator() => List.GetEnumerator();

    public Variant PickRandom() => List.Count == 0 ? default : List[(int)(GD.Randi() % (uint)List.Count)];
    public void Reverse() => List.Reverse();
    public void Shuffle()
    {
        for (int i = List.Count - 1; i > 0; i--)
        {
            int j = (int)(GD.Randi() % (uint)(i + 1));
            (List[i], List[j]) = (List[j], List[i]);
        }
    }
    public Variant Pop() { if (List.Count == 0) return default; Variant v = List[^1]; List.RemoveAt(List.Count - 1); return v; }
    public Variant PopFront() { if (List.Count == 0) return default; Variant v = List[0]; List.RemoveAt(0); return v; }
    public void PushFront(Variant v) => List.Insert(0, v);
    public Variant Front() => List.Count == 0 ? default : List[0];
    public Variant Back() => List.Count == 0 ? default : List[^1];
    public bool Has(Variant v) => List.Contains(v);
    public Error Resize(int size)
    {
        if (size < 0) return Error.InvalidParameter;
        while (List.Count > size) List.RemoveAt(List.Count - 1);
        while (List.Count < size) List.Add(default);
        return Error.Ok;
    }
    public void Sort() => List.Sort(static (a, b) => Compare(a, b));
    internal static int Compare(Variant a, Variant b) =>
        a.IsNumeric && b.IsNumeric ? a.AsDouble().CompareTo(b.AsDouble()) : string.CompareOrdinal(a.AsString(), b.AsString());
    public Array Slice(int begin, int end = int.MaxValue)
    {
        int n = List.Count;
        if (begin < 0) begin += n;
        if (end < 0) end += n;
        begin = Math.Clamp(begin, 0, n);
        end = Math.Clamp(end, begin, n);
        return new Array(List.GetRange(begin, end - begin));
    }
    public Array Duplicate(bool deep = false)
    {
        var copy = new Array();
        foreach (Variant v in List) copy.Add(deep ? v.Duplicate(true) : v);
        return copy;
    }
    public Array Map(Func<Variant, Variant> f) => new(List.Select(f));
    public Array Filter(Func<Variant, bool> f) => new(List.Where(f));

    public override string ToString()
    {
        var sb = new StringBuilder("[");
        for (int i = 0; i < List.Count; i++) { if (i > 0) sb.Append(", "); sb.Append(List[i].AsString()); }
        return sb.Append(']').ToString();
    }
}

/// <summary>A typed view of an <see cref="Array"/> (Godot's Array&lt;T&gt;).</summary>
public sealed class Array<T> : IList<T>, IReadOnlyList<T>
{
    private readonly Array _a;

    public Array() => _a = new Array();
    public Array(IEnumerable<T> collection) { _a = new Array(); foreach (T t in collection) _a.Add(Variant.From(t)); }
    public Array(Array array) => _a = array;

    public static explicit operator Array(Array<T> from) => from._a;
    public static explicit operator Array<T>(Array from) => new(from);

    public int Count => _a.Count;
    public bool IsReadOnly => false;
    public T this[int index]
    {
        get => _a[index].As<T>();
        set => _a[index] = Variant.From(value);
    }

    public void Add(T item) => _a.Add(Variant.From(item));
    public void Clear() => _a.Clear();
    public bool Contains(T item) => _a.Contains(Variant.From(item));
    public void CopyTo(T[] array, int arrayIndex) { for (int i = 0; i < _a.Count; i++) array[arrayIndex + i] = this[i]; }
    public int IndexOf(T item) => _a.IndexOf(Variant.From(item));
    public void Insert(int index, T item) => _a.Insert(index, Variant.From(item));
    public bool Remove(T item) => _a.Remove(Variant.From(item));
    public void RemoveAt(int index) => _a.RemoveAt(index);
    public IEnumerator<T> GetEnumerator() { for (int i = 0; i < _a.Count; i++) yield return this[i]; }
    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    public T PickRandom() => _a.PickRandom().As<T>();
    public void Shuffle() => _a.Shuffle();
    public void Reverse() => _a.Reverse();
    public void Sort() => _a.Sort();
    public Array<T> Duplicate(bool deep = false) => new(_a.Duplicate(deep));
    public override string ToString() => _a.ToString();
}

/// <summary>A reference-type dictionary of Variants that keeps insertion order (Godot's Dictionary).</summary>
public sealed class Dictionary : IDictionary<Variant, Variant>, IReadOnlyDictionary<Variant, Variant>
{
    private readonly List<KeyValuePair<Variant, Variant>> _items = new();
    private readonly System.Collections.Generic.Dictionary<Variant, int> _index = new();

    public Dictionary() { }
    public Dictionary(IDictionary<Variant, Variant> dictionary) { foreach (var kv in dictionary) this[kv.Key] = kv.Value; }

    public int Count => _items.Count;
    public bool IsReadOnly => false;
    public ICollection<Variant> Keys => _items.Select(kv => kv.Key).ToList();
    public ICollection<Variant> Values => _items.Select(kv => kv.Value).ToList();
    IEnumerable<Variant> IReadOnlyDictionary<Variant, Variant>.Keys => Keys;
    IEnumerable<Variant> IReadOnlyDictionary<Variant, Variant>.Values => Values;

    public Variant this[Variant key]
    {
        get => _index.TryGetValue(key, out int i) ? _items[i].Value : throw new KeyNotFoundException($"The key was not found: {key.AsString()}");
        set
        {
            if (_index.TryGetValue(key, out int i)) _items[i] = new KeyValuePair<Variant, Variant>(_items[i].Key, value);
            else { _index[key] = _items.Count; _items.Add(new KeyValuePair<Variant, Variant>(key, value)); }
        }
    }

    public void Add(Variant key, Variant value)
    {
        if (_index.ContainsKey(key)) throw new ArgumentException("An element with the same key already exists.");
        this[key] = value;
    }
    public void Add(KeyValuePair<Variant, Variant> item) => Add(item.Key, item.Value);
    public void Clear() { _items.Clear(); _index.Clear(); }
    public bool Contains(KeyValuePair<Variant, Variant> item) => _index.TryGetValue(item.Key, out int i) && _items[i].Value == item.Value;
    public bool ContainsKey(Variant key) => _index.ContainsKey(key);
    public bool Has(Variant key) => _index.ContainsKey(key);
    public void CopyTo(KeyValuePair<Variant, Variant>[] array, int arrayIndex) => _items.CopyTo(array, arrayIndex);

    public bool Remove(Variant key)
    {
        if (!_index.TryGetValue(key, out int at)) return false;
        _items.RemoveAt(at);
        _index.Remove(key);
        for (int i = at; i < _items.Count; i++) _index[_items[i].Key] = i;
        return true;
    }
    public bool Remove(KeyValuePair<Variant, Variant> item) => Contains(item) && Remove(item.Key);
    public bool TryGetValue(Variant key, out Variant value)
    {
        if (_index.TryGetValue(key, out int i)) { value = _items[i].Value; return true; }
        value = default;
        return false;
    }
    public Variant Get(Variant key, Variant @default = default) => TryGetValue(key, out Variant v) ? v : @default;
    public IEnumerator<KeyValuePair<Variant, Variant>> GetEnumerator() => _items.ToList().GetEnumerator();
    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    public void Merge(Dictionary other, bool overwrite = false)
    {
        foreach (var kv in other._items)
            if (overwrite || !ContainsKey(kv.Key)) this[kv.Key] = kv.Value;
    }
    public Array KeysArray() => new(_items.Select(kv => kv.Key));
    public Array ValuesArray() => new(_items.Select(kv => kv.Value));
    public Dictionary Duplicate(bool deep = false)
    {
        var d = new Dictionary();
        foreach (var kv in _items) d[kv.Key] = deep ? kv.Value.Duplicate(true) : kv.Value;
        return d;
    }

    public override string ToString()
    {
        if (_items.Count == 0) return "{ }";
        return "{ " + string.Join(", ", _items.Select(kv => $"{kv.Key.AsString()}: {kv.Value.AsString()}")) + " }";
    }
}

/// <summary>A typed view of a <see cref="Dictionary"/> (Godot's Dictionary&lt;K,V&gt;).</summary>
public sealed class Dictionary<TKey, TValue>
    : IDictionary<TKey, TValue>, IReadOnlyDictionary<TKey, TValue> where TKey : notnull
{
    private readonly Dictionary _d;

    public Dictionary() => _d = new Dictionary();
    public Dictionary(Dictionary dictionary) => _d = dictionary;

    public static explicit operator Dictionary(Dictionary<TKey, TValue> from) => from._d;
    public static explicit operator Dictionary<TKey, TValue>(Dictionary from) => new(from);

    public int Count => _d.Count;
    public bool IsReadOnly => false;
    public ICollection<TKey> Keys => _d.Keys.Select(k => k.As<TKey>()).ToList();
    public ICollection<TValue> Values => _d.Values.Select(v => v.As<TValue>()).ToList();
    IEnumerable<TKey> IReadOnlyDictionary<TKey, TValue>.Keys => Keys;
    IEnumerable<TValue> IReadOnlyDictionary<TKey, TValue>.Values => Values;

    public TValue this[TKey key]
    {
        get => _d[Variant.From(key)].As<TValue>();
        set => _d[Variant.From(key)] = Variant.From(value);
    }

    public void Add(TKey key, TValue value) => _d.Add(Variant.From(key), Variant.From(value));
    public void Add(KeyValuePair<TKey, TValue> item) => Add(item.Key, item.Value);
    public void Clear() => _d.Clear();
    public bool Contains(KeyValuePair<TKey, TValue> item) => _d.Contains(new KeyValuePair<Variant, Variant>(Variant.From(item.Key), Variant.From(item.Value)));
    public bool ContainsKey(TKey key) => _d.ContainsKey(Variant.From(key));
    public void CopyTo(KeyValuePair<TKey, TValue>[] array, int arrayIndex) { int i = arrayIndex; foreach (var kv in this) array[i++] = kv; }
    public bool Remove(TKey key) => _d.Remove(Variant.From(key));
    public bool Remove(KeyValuePair<TKey, TValue> item) => Contains(item) && Remove(item.Key);
    public bool TryGetValue(TKey key, out TValue value)
    {
        if (_d.TryGetValue(Variant.From(key), out Variant v)) { value = v.As<TValue>(); return true; }
        value = default!;
        return false;
    }
    public IEnumerator<KeyValuePair<TKey, TValue>> GetEnumerator()
    {
        foreach (var kv in _d) yield return new KeyValuePair<TKey, TValue>(kv.Key.As<TKey>(), kv.Value.As<TValue>());
    }
    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    public override string ToString() => _d.ToString();
}
