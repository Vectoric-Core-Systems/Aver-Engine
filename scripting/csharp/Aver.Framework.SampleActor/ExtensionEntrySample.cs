// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
using Aver.Framework.Hosting;

namespace Aver.Framework.SampleActor;

/// <summary>A managed extension entry for tests/extensions' ManagedExtensionTest: it names itself in the log and declares the
/// actors it chooses with a predicate, not an actor base. Not a project script (it is not an actor).</summary>
public sealed class SampleExtensionEntry : IAverExtension
{
    /// <summary>Logs what the context told it, then declares <see cref="Sphere"/> only.</summary>
    public void Load(ExtensionContext context)
    {
        context.Log($"entry ran: name={context.Name} version={context.Version} assemblies={context.Assemblies.Count} dir={context.Directory}");
        context.DeclareActors(t => t == typeof(Sphere));
    }
}

/// <summary>An entry that throws, to prove the extension is skipped with one log line.</summary>
public sealed class SampleThrowingEntry : IAverExtension
{
    /// <summary>Always throws.</summary>
    public void Load(ExtensionContext context) => throw new InvalidOperationException("entry refused to start");
}
