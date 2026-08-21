namespace Lox;

/// <summary>
/// Equality is identity, not structural (<see cref="object"/>'s default is
/// left alone on purpose): two separate constructions of the same variant
/// with the same payload are never equal - <c>Green() == Green()</c> is
/// <c>false</c> - per bytecode-translation-problems.md's "construct by
/// identity" finding.
/// </summary>
public sealed class LoxEnum {
    public readonly LoxEnumCtor Ctor;
    public readonly object[] Payload;

    public LoxEnum(LoxEnumCtor ctor, object[] payload) {
        Ctor = ctor;
        Payload = payload;
    }
}
