package org.jetbrains.kotlin.backend.konan.ir.interop.cstruct

import org.jetbrains.kotlin.backend.konan.InteropBuiltIns
import org.jetbrains.kotlin.backend.konan.RuntimeNames
import org.jetbrains.kotlin.backend.konan.ir.interop.DescriptorToIrTranslationMixin
import org.jetbrains.kotlin.descriptors.CallableMemberDescriptor
import org.jetbrains.kotlin.descriptors.ClassDescriptor
import org.jetbrains.kotlin.descriptors.PropertyDescriptor
import org.jetbrains.kotlin.ir.builders.irBlockBody
import org.jetbrains.kotlin.ir.builders.irGet
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.descriptors.IrBuiltIns
import org.jetbrains.kotlin.ir.expressions.impl.IrDelegatingConstructorCallImpl
import org.jetbrains.kotlin.ir.expressions.impl.IrInstanceInitializerCallImpl
import org.jetbrains.kotlin.ir.util.*

internal class CStructClassGenerator(
        override val symbolTable: SymbolTable,
        override val irBuiltIns: IrBuiltIns,
        override val typeTranslator: TypeTranslator,
        private val interopBuiltIns: InteropBuiltIns
) : DescriptorToIrTranslationMixin {

    private val companionGenerator = CStructVarCompanionGenerator(symbolTable, irBuiltIns, typeTranslator, interopBuiltIns)

    fun findOrGenerateCStruct(classDescriptor: ClassDescriptor, parent: IrDeclarationContainer): IrClass {
        val irClassSymbol = symbolTable.referenceClass(classDescriptor)
        return if (!irClassSymbol.isBound) {
            provideIrClassForCStruct(classDescriptor).also {
                it.patchDeclarationParents(parent)
                parent.declarations += it
            }
        } else {
            irClassSymbol.owner
        }
    }

    private fun provideIrClassForCStruct(descriptor: ClassDescriptor): IrClass =
            createClass(descriptor) { irClass ->
                irClass.addMember(createPrimaryConstructor(irClass))
                irClass.addMember(companionGenerator.generate(descriptor))
                descriptor.unsubstitutedMemberScope
                        .getContributedDescriptors()
                        .filterIsInstance<PropertyDescriptor>()
                        .filter { it.kind != CallableMemberDescriptor.Kind.FAKE_OVERRIDE }
                        .map(this::createStructProperty)
                        .forEach(irClass::addMember)
            }

    private fun createPrimaryConstructor(irClass: IrClass): IrConstructor {
        val irConstructor = createConstructor(irClass.descriptor.unsubstitutedPrimaryConstructor!!)
        val enumVarConstructorSymbol = symbolTable.referenceConstructor(
                interopBuiltIns.cStructVar.unsubstitutedPrimaryConstructor!!
        )
        irConstructor.body = irBuilder(irBuiltIns, irConstructor.symbol).irBlockBody {
            +IrDelegatingConstructorCallImpl(
                    startOffset, endOffset, context.irBuiltIns.unitType, enumVarConstructorSymbol
            ).also {
                it.putValueArgument(0, irGet(irConstructor.valueParameters[0]))
            }
            +IrInstanceInitializerCallImpl(
                    startOffset, endOffset,
                    symbolTable.referenceClass(irClass.descriptor),
                    context.irBuiltIns.unitType
            )
        }
        return irConstructor
    }

    private fun createStructProperty(propertyDescriptor: PropertyDescriptor): IrProperty {
        val irProperty = createProperty(propertyDescriptor)
        symbolTable.withScope(propertyDescriptor) {
            irProperty.getter = propertyDescriptor.getter?.let { getterDescriptor ->
                declareSimpleIrFunction(getterDescriptor).also { irGetter ->
                    irGetter.correspondingPropertySymbol = irProperty.symbol
                }
            }
        }
        return irProperty
    }
}