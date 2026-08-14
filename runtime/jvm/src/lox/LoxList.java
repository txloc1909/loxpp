package lox;

import java.util.ArrayList;
import java.util.List;

/** Identity equality (Lox default for List) is Object's default — do not add equals/hashCode. */
public final class LoxList {
    public final List<Object> elements = new ArrayList<>();
}
