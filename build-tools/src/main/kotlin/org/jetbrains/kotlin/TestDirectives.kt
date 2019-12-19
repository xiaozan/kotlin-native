/*
 * Copyright 2010-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

package org.jetbrains.kotlin

import org.gradle.api.Project
import java.nio.file.Paths
import java.util.regex.Pattern

private const val MODULE_DELIMITER = ",\\s*"
private val FILE_OR_MODULE_PATTERN: Pattern = Pattern.compile("(?://\\s*MODULE:\\s*([^()\\n]+)(?:\\(([^()]+(?:" +
        "$MODULE_DELIMITER[^()]+)*)\\))?\\s*(?:\\(([^()]+(?:$MODULE_DELIMITER[^()]+)*)\\))?\\s*)?//\\s*FILE:\\s*(.*)$",
        Pattern.MULTILINE)

/**
 * Creates files from the given source file that may contain different test directives.
 *
 * @return list of file names to be compiled
 */
fun buildCompileList(project: Project, source: String, outputDirectory: String): List<String> {
    val result = mutableListOf<String>()
    val srcFile = project.file(source)
    // Remove diagnostic parameters in external tests.
    val srcText = srcFile.readText().replace(Regex("<!.*?!>(.*?)<!>")) { match -> match.groupValues[1] }

    if (srcText.contains("// WITH_COROUTINES")) {
        val coroutineHelpers = "$outputDirectory/helpers.kt"
        val helper = TestFile("helpers.kt", coroutineHelpers, createTextForHelpers(true))
        helper.create()
        result.add(coroutineHelpers)
    }

    val matcher = FILE_OR_MODULE_PATTERN.matcher(srcText)
    if (!matcher.find()) {
        // There is only one file in the input
        registerKtFile(result, TestFile(srcFile.name, "$outputDirectory/${srcFile.name}", srcText))
    } else {
        // There are several files
        var processedChars = 0
        var hasModules = false
        var module: TestModule? = null
        while (true) {
            var moduleName = matcher.group(1)
            val moduleDependencies = matcher.group(2)
            val moduleFriends = matcher.group(3)

            if (moduleName != null) {
                moduleName = moduleName.trim { it <= ' ' }
                hasModules = true
                module = TestModule(moduleName, parseModuleList(moduleDependencies), parseModuleList(moduleFriends))
            }

            val fileName = matcher.group(4)
            val filePath = "$outputDirectory/$fileName"
            val start = processedChars
            val nextFileExists = matcher.find()
            val end = if (nextFileExists) matcher.start() else srcText.length
            val fileText = srcText.substring(start, end)
            processedChars = end
            val testFile = TestFile(fileName, filePath, fileText, module)
            registerKtFile(result, testFile)
            testFile.create()
            if (!nextFileExists) break
        }
    }
    return result
}

class TestModule(val name: String, val dependencies: List<String>, val friends: List<String>) {
    val testFiles = mutableListOf<TestFile>()
}

class TestFile(val name: String, val path: String, val text: String, val module: TestModule? = null) {
    fun create() = Paths.get(path).run {
        parent.toFile()
                .takeUnless { it.exists() }
                ?.mkdirs()
        toFile().writeText(text)
    }
}

fun parseModuleList(dependencies: String?): List<String> {
    return dependencies
            ?.split(Pattern.compile(MODULE_DELIMITER), 0)
            ?: emptyList()
}

internal fun registerKtFile(sourceFiles: MutableList<String>, file: TestFile) {
    if (file.path.endsWith(".kt")) {
        sourceFiles.add(file.path)
    }
}
