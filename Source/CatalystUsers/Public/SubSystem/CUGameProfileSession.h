#pragma once

#include "CCConnectTypes.h"
#include "Presentation/CFMagritteBinding.h"
#include "Subsystems/LocalPlayerSubsystem.h"

#include "CUGameProfileSession.generated.h"

class UCCSubsystem;

/** Supplies an authorized User graph for one local player's selected game. */
UCLASS()
class CATALYSTUSERS_API UCUGameProfileSession : public ULocalPlayerSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    TSharedPtr<ICFPlayerMagritteSource> PresentationSource() const { return Source; }

    // The game calls this after Users sign-in on the named Connect generation.
    // The installed operation reads the current game profile of that session.
    bool LoadCurrentGameProfile(FName Connection, uint64 AuthenticatedGeneration, uint16 Operation,
        TConstArrayView<uint8> Request, const FString& VerifiedUserId);
    /** Request an updated profile from the same authenticated connection. */
    bool RequestProfileUpdate(uint16 Operation, TConstArrayView<uint8> Request);
    void ClearProfile();

    TOptional<FCFMagritteBinding> CurrentUser() const;

private:
    bool QueueProfileGraph(uint16 Operation, TConstArrayView<uint8> Request, const FString& VerifiedUserId);
    UFUNCTION()
    void ConnectionChanged(FName Connection, ECCConnectState State, int32 Reason);

    TWeakObjectPtr<UCCSubsystem> Connect;
    TSharedPtr<ICFPlayerMagritteSource> Source;
    TOptional<FCFMagritteBinding> User;
    FName ActiveConnection;
    uint64 ActiveGeneration = 0;
    uint64 ActiveRequest = 0;
    uint64 LoadSerial = 0;
};
