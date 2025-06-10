/*
        Dawning
        DDL (Dumb Direct Language) + SOL

        A step between C and machine code,
        using SOL syntax and Dawning C Standard type system.

        Dawning SOL - sol (Sun) Dawn's simple language syntax
        Sol is a work in progress syntax system

        it's also experimental work on compiler stuff

        Dawn Larsson - Apache-2.0 license
*/

#include "library.c"

fn parse_sol(writer write, string_address string, string_address entry)
{
}

#ifndef COMPILER_LIB_MODE

const test_sol = 'main\n\tlog "testing"\n';

b32 main()
{
        parse_sol(test_sol, "main");
}

#endif