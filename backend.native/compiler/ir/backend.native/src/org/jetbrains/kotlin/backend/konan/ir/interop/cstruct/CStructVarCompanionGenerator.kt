package org.jetbrains.kotlin.backend.konan.ir.interop.cstruct

import org.jetbrains.kotlin.backend.konan.InteropBuiltIns
import org.jetbrains.kotlin.backend.konan.descriptors.getArgumentValueOrNull
import org.jetbrains.kotlin.backend.konan.ir.interop.DescriptorToIrTranslationMixin
import org.jetbrains.kotlin.descriptors.ClassDescriptor
import org.jetbrains.kotlin.ir.builders.irBlockBody
import org.jetbrains.kotlin.ir.builders.irInt
import org.jetbrains.kotlin.ir.builders.irLong
import org.jetbrains.kotlin.ir.declarations.IrClass
import org.jetbrains.kotlin.ir.declarations.IrConstructor
import org.jetbrains.kotlin.ir.declarations.addMember
import org.jetbrains.kotlin.ir.descriptors.IrBuiltIns
import org.jetbrains.kotlin.ir.expressions.impl.IrDelegatingConstructorCallImpl
import org.jetbrains.kotlin.ir.expressions.impl.IrInstanceInitializerCallImpl
import org.jetbrains.kotlin.ir.util.SymbolTable
import org.jetbrains.kotlin.ir.util.TypeTranslator
import org.jetbrains.kotlin.ir.util.irBuilder
import org.jetbrains.kotlin.name.FqName
import org.jetbrains.kotlin.psi2ir.generators.GeneratorContext

private val varTypeAnnotationFqName = FqName("kotlinx.cinterop.internal.CStruct.VarType")

internal class CStructVarCompanionGenerator(
        override val symbolTable: SymbolTable,
        override val irBuiltIns: IrBuiltIns,
        override val typeTranslator: TypeTranslator,
        private val interopBuiltIns: InteropBuiltIns
) : DescriptorToIrTranslationMixin {

    fun generate(structDescriptor: ClassDescriptor): IrClass =
        createClass(structDescriptor.companionObjectDescriptor!!) { companionIrClass ->
            val annotation = companionIrClass.descriptor.annotations
                    .findAnnotation(varTypeAnnotationFqName)!!
            val size = annotation.getArgumentValueOrNull<Long>("size")!!
            val align = annotation.getArgumentValueOrNull<Int>("align")!!
            companionIrClass.addMember(createCompanionConstructor(companionIrClass.descriptor, size, align))
        }

    private fun createCompanionConstructor(companionObjectDescriptor: ClassDescriptor, size: Long, align: Int): IrConstructor {
        val irConstructor = createConstructor(companionObjectDescriptor.unsubstitutedPrimaryConstructor!!)
        val superConstructorSymbol = symbolTable.referenceConstructor(interopBuiltIns.cStructVarType.unsubstitutedPrimaryConstructor!!)
        irConstructor.body = irBuilder(irBuiltIns, irConstructor.symbol).irBlockBody {
            +IrDelegatingConstructorCallImpl(
                    startOffset, endOffset, context.irBuiltIns.unitType,
                    superConstructorSymbol
            ).also {
                it.putValueArgument(0, irLong(size))
                it.putValueArgument(1, irInt(align))
            }
            +IrInstanceInitializerCallImpl(
                    startOffset, endOffset,
                    symbolTable.referenceClass(companionObjectDescriptor),
                    context.irBuiltIns.unitType
            )
        }
        return irConstructor
    }
}