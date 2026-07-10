/**************************************************************************/
/*  test_csharp_language.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_csharp_language)

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MONO_ENABLED

#include "modules/mono/csharp_script.h"

namespace TestCSharpLanguage {

TEST_SUITE("[CSharpLanguage]") {
	TEST_CASE("Modern multiline syntax does not produce false semicolon errors") {
		CSharpLanguage *language = CSharpLanguage::get_singleton();
		REQUIRE(language != nullptr);

		const String source = R"CSHARP(
#nullable enable
using System;

public partial class ModernSyntax
{
    private static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;

    private Error Fail(Error error, string message, Exception? exception = null)
    {
        _ = exception is null ? message : $"{message}: {exception.Message}";
        return error;
    }

    private static Error MapException(Exception exception, Error fallback)
    {
        return exception switch
        {
            UnauthorizedAccessException => Error.FileNoPermission,
            _ => fallback,
        };
    }
}
)CSHARP";

		List<ScriptLanguage::ScriptError> errors;
		CHECK(language->validate(source, "res://ModernSyntax.cs", nullptr, &errors));
		CHECK(errors.is_empty());
	}

	TEST_CASE("Mismatched brackets still produce structural errors") {
		CSharpLanguage *language = CSharpLanguage::get_singleton();
		REQUIRE(language != nullptr);

		const String source = "public class Broken { public void Run() { ] } }";
		List<ScriptLanguage::ScriptError> errors;
		CHECK_FALSE(language->validate(source, "res://Broken.cs", nullptr, &errors));
		CHECK_FALSE(errors.is_empty());
	}
}

} // namespace TestCSharpLanguage

#endif // MODULE_MONO_ENABLED
